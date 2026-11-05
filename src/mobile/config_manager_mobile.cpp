#include "core/config_manager.h"

#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "utils/file_utils.h"

namespace vxcore {

namespace {

bool IsStrictUtf8(const char *text) {
  if (!text || !*text) return false;
  const auto *p = reinterpret_cast<const unsigned char *>(text);
  while (*p) {
    const unsigned char lead = *p++;
    if (lead < 0x80) continue;
    unsigned int value;
    unsigned int remaining;
    unsigned int minimum;
    if (lead >= 0xc2 && lead <= 0xdf) {
      value = lead & 0x1f; remaining = 1; minimum = 0x80;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      value = lead & 0x0f; remaining = 2; minimum = 0x800;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      value = lead & 0x07; remaining = 3; minimum = 0x10000;
    } else {
      return false;
    }
    while (remaining--) {
      if ((*p & 0xc0) != 0x80) return false;
      value = (value << 6) | (*p++ & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
      return false;
    }
  }
  return true;
}

#ifdef _WIN32
struct DirectoryHandle {
  HANDLE value;
  ~DirectoryHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

std::filesystem::path WithoutExtendedPrefix(const std::wstring &path) {
  if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
    return std::filesystem::path(L"\\\\" + path.substr(8));
  }
  if (path.compare(0, 4, L"\\\\?\\") == 0 && path.size() > 6 && path[5] == L':') {
    return std::filesystem::path(path.substr(4));
  }
  return std::filesystem::path(path);
}
#endif

VxCoreError CanonicalDirectory(const std::filesystem::path &path,
                               std::filesystem::path &out, bool writable) {
#ifdef _WIN32
  const DWORD access = FILE_READ_ATTRIBUTES |
      (writable ? FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY : 0);
  DirectoryHandle handle{CreateFileW(path.c_str(), access,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
  if (handle.value == INVALID_HANDLE_VALUE) return VXCORE_ERR_IO;
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle.value, &info) ||
      !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return VXCORE_ERR_IO;
  const DWORD size = GetFinalPathNameByHandleW(handle.value, nullptr, 0, FILE_NAME_NORMALIZED);
  if (!size) return VXCORE_ERR_IO;
  std::wstring final_path(size, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(handle.value, final_path.data(), size,
                                               FILE_NAME_NORMALIZED);
  if (!length || length >= size) return VXCORE_ERR_IO;
  final_path.resize(length);
  out = WithoutExtendedPrefix(final_path);
#else
  std::error_code ec;
  const auto status = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::is_directory(status)) return VXCORE_ERR_IO;
  using P = std::filesystem::perms;
  // Ancestors need traversal, not enumeration (Android /data is search-only
  // for app UIDs). The effective storage ancestor must also be readable/writable.
  if ((status.permissions() & (P::owner_exec | P::group_exec | P::others_exec)) == P::none ||
      (writable && ((status.permissions() & (P::owner_read | P::group_read | P::others_read)) == P::none ||
                    (status.permissions() & (P::owner_write | P::group_write | P::others_write)) == P::none)) ||
      ::access(path.c_str(), X_OK | (writable ? R_OK | W_OK : 0)) != 0) return VXCORE_ERR_IO;
  out = std::filesystem::canonical(path, ec);
  if (ec) return VXCORE_ERR_IO;
#endif
  return VXCORE_OK;
}

VxCoreError ResolveDataRoot(const char *text, std::filesystem::path &out) {
  auto path = PathFromUtf8(text);
#ifdef _WIN32
  path.make_preferred();
  path = WithoutExtendedPrefix(path.native());
  // Device/extended namespaces and Win32 trimmed names must not bypass alias checks.
  if (path.native().compare(0, 4, L"\\\\.\\") == 0 ||
      path.native().compare(0, 4, L"\\\\?\\") == 0) return VXCORE_ERR_INVALID_PARAM;
  for (const auto &part : path.relative_path()) {
    const auto &name = part.native();
    if (name.empty() || name == L"." || name == L"..") continue;
    if (name.back() == L'.' || name.back() == L' ' ||
        name.find_first_of(L":*?\"<>|") != std::wstring::npos) return VXCORE_ERR_INVALID_PARAM;
  }
#endif
  if (!path.is_absolute()) return VXCORE_ERR_INVALID_PARAM;
  std::filesystem::path resolved;
  VxCoreError err = CanonicalDirectory(path.root_path(), resolved, false);
  if (err != VXCORE_OK) return err;
  // Resolve components in order: normalizing '..' before a symlink would select
  // a different directory. Missing components are retained without creating them.
  for (const auto &part : path.relative_path()) {
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      resolved = resolved.parent_path();
      continue;
    }
    auto candidate = resolved / part;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(candidate, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return VXCORE_ERR_IO;
    if (!std::filesystem::exists(status)) {
      resolved = std::move(candidate);
      continue;
    }
    err = CanonicalDirectory(candidate, resolved, false);
    if (err != VXCORE_OK) return err;
  }
  auto ancestor = resolved;
  for (;;) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(ancestor, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return VXCORE_ERR_IO;
    if (std::filesystem::exists(status)) break;
    ancestor = ancestor.parent_path();
  }
  std::filesystem::path canonical_ancestor;
  err = CanonicalDirectory(ancestor, canonical_ancestor, true);
  if (err != VXCORE_OK) return err;
  out = std::move(resolved);
  return VXCORE_OK;
}

bool ContainsDataRoot(const std::filesystem::path &parent, const std::filesystem::path &child) {
  auto p = parent.begin();
  auto c = child.begin();
  for (; p != parent.end(); ++p, ++c) {
    if (c == child.end()) return false;
#ifdef _WIN32
    const auto &left = p->native();
    const auto &right = c->native();
    if (CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                             static_cast<int>(right.size()), TRUE) != CSTR_EQUAL) return false;
#else
    if (*p != *c) return false;
#endif
  }
  return true;
}

}  // namespace

ConfigManager::ConfigManager(std::filesystem::path app_data_path,
                             std::filesystem::path local_data_path)
    : app_data_path_(std::move(app_data_path)), local_data_path_(std::move(local_data_path)),
      load_session_config_(false) {}

VxCoreError ConfigManager::ValidateDataPaths(const char *app_data_path, const char *local_data_path,
                                            std::filesystem::path &out_app,
                                            std::filesystem::path &out_local) {
  if (!IsStrictUtf8(app_data_path) || !IsStrictUtf8(local_data_path)) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  auto err = ResolveDataRoot(app_data_path, out_app);
  if (err != VXCORE_OK) return err;
  err = ResolveDataRoot(local_data_path, out_local);
  if (err != VXCORE_OK) return err;
  if (ContainsDataRoot(out_app, out_local) || ContainsDataRoot(out_local, out_app)) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  return VXCORE_OK;
}

}  // namespace vxcore
