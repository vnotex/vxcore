#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "file_utils.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(VXCORE_BUILD_DLL)
#include "logger.h"
#endif

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#endif

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

#ifndef VXCORE_LOG_DEBUG
#define VXCORE_LOG_DEBUG(...) ((void)0)
#endif

#ifndef VXCORE_LOG_WARN
#define VXCORE_LOG_WARN(...) ((void)0)
#endif

namespace vxcore {

static constexpr char kPathSeparator = '/';

#ifdef _WIN32
static std::string WideToUtf8(const std::wstring &wide_str) {
  if (wide_str.empty()) {
    return {};
  }

  int utf8_size =
      WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (utf8_size <= 0) {
    return {};
  }

  std::string utf8_str(static_cast<size_t>(utf8_size), '\0');
  int converted = WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), -1, utf8_str.data(), utf8_size,
                                      nullptr, nullptr);
  if (converted <= 0) {
    return {};
  }

  utf8_str.resize(static_cast<size_t>(utf8_size - 1));
  return utf8_str;
}
#endif

std::filesystem::path PathFromUtf8(const std::string &utf8_str) {
  if (utf8_str.empty()) {
    return {};
  }

#ifdef _WIN32
  int wide_size = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, nullptr, 0);
  if (wide_size <= 0) {
    return {};
  }

  int wide_len = wide_size - 1;
  std::wstring wide_str(static_cast<size_t>(wide_size), L'\0');
  int converted = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, wide_str.data(), wide_size);
  if (converted <= 0) {
    return {};
  }

  wide_str.resize(static_cast<size_t>(wide_len));
  VXCORE_LOG_DEBUG("PathFromUtf8: input_bytes=%zu, wide_chars=%d", utf8_str.size(), wide_len);
  return std::filesystem::path(wide_str);
#else
  return std::filesystem::path(utf8_str);
#endif
}

std::string PathToUtf8(const std::filesystem::path &path) {
#ifdef _WIN32
  return WideToUtf8(path.wstring());
#else
  return path.string();
#endif
}

std::string PathToGenericUtf8(const std::filesystem::path &path) {
#ifdef _WIN32
  return WideToUtf8(path.generic_wstring());
#else
  return path.generic_string();
#endif
}

std::string ConcatenatePaths(const std::string &parent_path, const std::string &child_name) {
  if (parent_path.empty() || parent_path == ".") {
    return child_name;
  } else {
    return parent_path + kPathSeparator + child_name;
  }
}

namespace {
#ifdef _WIN32
int64_t FileTimeToUnixMillis(const FILETIME &ft) {
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  if (u.QuadPart == 0) return 0;
  // FILETIME counts 100-ns ticks since 1601-01-01; offset to the Unix epoch.
  constexpr int64_t kEpochOffsetTicks = 116444736000000000LL;
  return (static_cast<int64_t>(u.QuadPart) - kEpochOffsetTicks) / 10000LL;  // 100-ns -> ms
}
#endif
}  // namespace

namespace {
using FsPath = std::filesystem::path;
using PathView = std::basic_string_view<FsPath::value_type>;

[[noreturn]] void PrivateFailure(std::error_code error) {
  // Never attach paths: these exceptions can cross credential/state boundaries.
  throw std::filesystem::filesystem_error("Private filesystem operation failed", error);
}
[[noreturn]] void PrivateFailure(std::errc error) {
  PrivateFailure(std::make_error_code(error));
}

bool PrivateSeparator(FsPath::value_type ch) {
#ifdef _WIN32
  return ch == L'/' || ch == L'\\';
#else
  return ch == '/';
#endif
}

void ValidatePrivateComponent(PathView part) {
  if (part.empty() || (part.size() == 1 && part[0] == '.') ||
      (part.size() == 2 && part[0] == '.' && part[1] == '.')) {
    PrivateFailure(std::errc::invalid_argument);
  }
  for (size_t i = 0; i < part.size(); ++i) {
    const auto ch = part[i];
    if (ch == 0 || ch == ':' || ch == '\\' || ch == '/') {
      PrivateFailure(std::errc::invalid_argument);
    }
#ifdef _WIN32
    if (ch < 32 || ch == L'<' || ch == L'>' || ch == L'"' || ch == L'|' ||
        ch == L'?' || ch == L'*') {
      PrivateFailure(std::errc::invalid_argument);
    }
    if (ch >= 0xd800 && ch <= 0xdbff) {
      if (++i == part.size() || part[i] < 0xdc00 || part[i] > 0xdfff) {
        PrivateFailure(std::errc::invalid_argument);
      }
    } else if (ch >= 0xdc00 && ch <= 0xdfff) {
      PrivateFailure(std::errc::invalid_argument);
    }
#endif
  }
#ifdef _WIN32
  if (part.size() > 255 || part.back() == L'.' || part.back() == L' ') {
    PrivateFailure(std::errc::invalid_argument);
  }
  auto stem = part.substr(0, part.find(L'.'));
  while (!stem.empty() && stem.back() == L' ') stem.remove_suffix(1);
  const auto equals_ascii = [](PathView value, std::wstring_view name) {
    if (value.size() != name.size()) return false;
    for (size_t i = 0; i < value.size(); ++i) {
      auto ch = value[i];
      if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
      if (ch != name[i]) return false;
    }
    return true;
  };
  if (equals_ascii(stem, L"CON") || equals_ascii(stem, L"PRN") ||
      equals_ascii(stem, L"AUX") || equals_ascii(stem, L"NUL") ||
      equals_ascii(stem, L"CONIN$") || equals_ascii(stem, L"CONOUT$") ||
      equals_ascii(stem, L"CLOCK$") ||
      (stem.size() == 4 &&
       (equals_ascii(stem.substr(0, 3), L"COM") || equals_ascii(stem.substr(0, 3), L"LPT")) &&
       ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == 0xb9 ||
        stem[3] == 0xb2 || stem[3] == 0xb3))) {
    PrivateFailure(std::errc::invalid_argument);
  }
#endif
}

// Returns the end of the logical physical root (drive root or complete UNC share).
// Work on the original native string: filesystem iterators can hide empty components.
size_t ValidatePrivateSpelling(const FsPath &path) {
  const PathView value(path.native());
  size_t root_end = 0;
#ifdef _WIN32
  if (value.size() >= 3 &&
      ((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')) &&
      value[1] == L':' && PrivateSeparator(value[2])) {
    root_end = 3;
  } else if (value.size() >= 5 && PrivateSeparator(value[0]) && PrivateSeparator(value[1])) {
    size_t server_end = 2;
    while (server_end < value.size() && !PrivateSeparator(value[server_end])) ++server_end;
    ValidatePrivateComponent(value.substr(2, server_end - 2));
    if (server_end == value.size()) PrivateFailure(std::errc::invalid_argument);
    root_end = server_end + 1;
    while (root_end < value.size() && !PrivateSeparator(value[root_end])) ++root_end;
    ValidatePrivateComponent(value.substr(server_end + 1, root_end - server_end - 1));
    if (root_end < value.size()) ++root_end;
  } else {
    PrivateFailure(std::errc::invalid_argument);
  }
#else
  if (value.empty() || value.front() != '/') PrivateFailure(std::errc::invalid_argument);
  root_end = 1;
#endif
  for (size_t start = root_end; start < value.size();) {
    size_t end = start;
    while (end < value.size() && !PrivateSeparator(value[end])) ++end;
    ValidatePrivateComponent(value.substr(start, end - start));
    if (end == value.size()) break;
    start = end + 1;
    if (start == value.size()) PrivateFailure(std::errc::invalid_argument);
  }
  return root_end;
}

VxCoreError PrivateMappedError(const std::error_code &error) {
#ifdef _WIN32
  SetLastError(error.category() == std::system_category() ? static_cast<DWORD>(error.value()) :
               error == std::errc::invalid_argument ? ERROR_INVALID_PARAMETER :
               error == std::errc::not_enough_memory ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_DATA);
#endif
  if (error == std::errc::not_enough_memory) return VXCORE_ERR_OUT_OF_MEMORY;
  return error == std::errc::invalid_argument ? VXCORE_ERR_INVALID_PARAM : VXCORE_ERR_IO;
}

struct PrivateNode {
  private_io::PathStatus status;
  bool reparse = false;
#ifdef _WIN32
  BY_HANDLE_FILE_INFORMATION info{};
  FILE_ID_INFO id{};
#else
  struct stat info{};
#endif
};

// The caller checked the ancestors. The final object itself is opened/stat'ed
// without following it, using exactly the spelling used by every private open.
PrivateNode ProbePrivateNode(const FsPath &path, bool identity = false) {
  PrivateNode node;
  const NativePathArgument argument(path, FileAccess::kPrivate);
#ifdef _WIN32
  HANDLE handle = CreateFileW(argument.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return node;
    PrivateFailure(std::error_code(static_cast<int>(error), std::system_category()));
  }
  DWORD error = ERROR_SUCCESS;
  if (!GetFileInformationByHandle(handle, &node.info)) error = GetLastError();
  else if (GetFileType(handle) != FILE_TYPE_DISK ||
           (node.info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) != 0) error = ERROR_INVALID_DATA;
  else if (identity && !GetFileInformationByHandleEx(handle, FileIdInfo, &node.id, sizeof(node.id))) {
    error = GetLastError();
  }
  if (!CloseHandle(handle) && error == ERROR_SUCCESS) error = GetLastError();
  if (error != ERROR_SUCCESS) {
    PrivateFailure(std::error_code(static_cast<int>(error), std::system_category()));
  }
  node.reparse = (node.info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  node.status.directory = (node.info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  node.status.regular = !node.status.directory;
  node.status.size = (static_cast<uint64_t>(node.info.nFileSizeHigh) << 32) | node.info.nFileSizeLow;
  node.status.modified = (static_cast<uint64_t>(node.info.ftLastWriteTime.dwHighDateTime) << 32) |
                         node.info.ftLastWriteTime.dwLowDateTime;
#else
  (void)identity;
  if (::lstat(argument.c_str(), &node.info) != 0) {
    const int error = errno;
    if (error == ENOENT) return node;
    PrivateFailure(std::error_code(error, std::generic_category()));
  }
  node.reparse = S_ISLNK(node.info.st_mode);
  node.status.directory = S_ISDIR(node.info.st_mode);
  node.status.regular = S_ISREG(node.info.st_mode);
  if (!node.reparse && !node.status.directory && !node.status.regular) {
    PrivateFailure(std::errc::operation_not_supported);
  }
  node.status.size = static_cast<uint64_t>(node.info.st_size);
#ifdef __APPLE__
  node.status.modified = static_cast<uint64_t>(node.info.st_mtimespec.tv_sec) * 1000000000ULL +
                         static_cast<uint64_t>(node.info.st_mtimespec.tv_nsec);
#else
  node.status.modified = static_cast<uint64_t>(node.info.st_mtim.tv_sec) * 1000000000ULL +
                         static_cast<uint64_t>(node.info.st_mtim.tv_nsec);
#endif
#endif
  node.status.exists = true;
  return node;
}

void RequireOrdinary(const PrivateNode &node) {
  if (node.reparse) PrivateFailure(std::errc::too_many_symbolic_link_levels);
}

FileIdentity PrivateIdentity(const PrivateNode &node) {
  FileIdentity identity;
  if (!node.status.exists) PrivateFailure(std::errc::no_such_file_or_directory);
  RequireOrdinary(node);
#ifdef _WIN32
  identity.device = node.id.VolumeSerialNumber;
  std::memcpy(&identity.low, node.id.FileId.Identifier, sizeof(identity.low));
  std::memcpy(&identity.high, node.id.FileId.Identifier + sizeof(identity.low), sizeof(identity.high));
  identity.links = node.info.nNumberOfLinks;
#else
  identity.device = static_cast<uint64_t>(node.info.st_dev);
  identity.low = static_cast<uint64_t>(node.info.st_ino);
  identity.links = static_cast<uint64_t>(node.info.st_nlink);
#endif
  identity.directory = node.status.directory;
  if (identity.device == 0 || (identity.low == 0 && identity.high == 0) || identity.links == 0) {
    PrivateFailure(std::errc::io_error);
  }
  return identity;
}

FileIdentity PrivateOpenFileIdentity(int fd) {
  PrivateNode node;
#ifdef _WIN32
  const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_DISK) {
    PrivateFailure(std::errc::io_error);
  }
  if (!GetFileInformationByHandle(handle, &node.info) ||
      !GetFileInformationByHandleEx(handle, FileIdInfo, &node.id, sizeof(node.id))) {
    PrivateFailure(std::error_code(static_cast<int>(GetLastError()), std::system_category()));
  }
  if ((node.info.dwFileAttributes &
       (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
    PrivateFailure(std::errc::io_error);
  }
#else
  if (::fstat(fd, &node.info) != 0) PrivateFailure(std::error_code(errno, std::generic_category()));
  if (!S_ISREG(node.info.st_mode)) PrivateFailure(std::errc::io_error);
#endif
  node.status.exists = node.status.regular = true;
  return PrivateIdentity(node);
}

bool CreatePrivateLeaf(const FsPath &path) {
  const NativePathArgument argument(path, FileAccess::kPrivate);
#ifdef _WIN32
  if (CreateDirectoryW(argument.c_str(), nullptr)) return true;
  const DWORD error = GetLastError();
  if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
    PrivateFailure(std::error_code(static_cast<int>(error), std::system_category()));
  }
#else
  if (::mkdir(argument.c_str(), S_IRWXU) == 0) return true;
  const int error = errno;
  if (error != EEXIST) PrivateFailure(std::error_code(error, std::generic_category()));
#endif
  return false;
}

PrivateNode WalkPrivatePath(const FsPath &path, bool create = false,
                            bool allow_final_reparse = false, bool identity = false) {
  const size_t root_end = ValidatePrivateSpelling(path);
  const auto &value = path.native();
  size_t end = root_end;
  FsPath component;
  for (;;) {
    component = FsPath(value.substr(0, end));
    auto node = ProbePrivateNode(component, identity && end == value.size());
    if (!node.status.exists && create && end != root_end) {
      CreatePrivateLeaf(component);
      node = ProbePrivateNode(component);
      if (!node.status.exists) PrivateFailure(std::errc::no_such_file_or_directory);
    }
    if (!node.status.exists) return node;
    if (!allow_final_reparse || end != value.size()) RequireOrdinary(node);
    if (end == value.size()) {
      if (create && !node.status.directory) PrivateFailure(std::errc::not_a_directory);
      return node;
    }
    if (!node.status.directory) PrivateFailure(std::errc::not_a_directory);
    if (end != root_end) ++end;
    while (end < value.size() && !PrivateSeparator(value[end])) ++end;
  }
}
}  // namespace

NativePathArgument::NativePathArgument(const std::filesystem::path &path, FileAccess access)
    : borrowed_(path.c_str()) {
  if (access != FileAccess::kPrivate) return;
  ValidatePrivateSpelling(path);
#ifdef _WIN32
  const auto &logical = path.native();
  const bool unc = PrivateSeparator(logical[0]);
  owned_.reserve(logical.size() + (unc ? 6 : 4));
  owned_ = unc ? L"\\\\?\\UNC\\" : L"\\\\?\\";
  owned_.append(logical, unc ? 2 : 0, std::wstring::npos);
  std::replace(owned_.begin(), owned_.end(), L'/', L'\\');
  borrowed_ = nullptr;
#endif
}

const std::filesystem::path::value_type *NativePathArgument::c_str() const noexcept {
#ifdef _WIN32
  return borrowed_ ? borrowed_ : owned_.c_str();
#else
  return borrowed_;
#endif
}

namespace private_io {
PathStatus Status(const FsPath &path) { return WalkPrivatePath(path).status; }
bool Exists(const FsPath &path) { return Status(path).exists; }
bool IsDirectory(const FsPath &path) { return Status(path).directory; }
bool IsRegularFile(const FsPath &path) { return Status(path).regular; }

void Validate(const FsPath &path, bool allow_missing) {
  if (!Status(path).exists && !allow_missing) PrivateFailure(std::errc::no_such_file_or_directory);
}

uint64_t FileSize(const FsPath &path) {
  const auto status = Status(path);
  if (!status.exists) PrivateFailure(std::errc::no_such_file_or_directory);
  if (!status.regular) PrivateFailure(std::errc::is_a_directory);
  return status.size;
}

uint64_t ModifiedTime(const FsPath &path) {
  const auto status = Status(path);
  if (!status.exists) PrivateFailure(std::errc::no_such_file_or_directory);
  return status.modified;
}

bool MakeDirectory(const FsPath &path) {
  const size_t root_end = ValidatePrivateSpelling(path);
  if (root_end == path.native().size()) {
    if (!IsDirectory(path)) PrivateFailure(std::errc::no_such_file_or_directory);
    return false;
  }
  const auto parent = Status(path.parent_path());
  if (!parent.exists) PrivateFailure(std::errc::no_such_file_or_directory);
  if (!parent.directory) PrivateFailure(std::errc::not_a_directory);
  const bool created = CreatePrivateLeaf(path);
  const auto node = ProbePrivateNode(path);
  RequireOrdinary(node);
  if (!node.status.exists || !node.status.directory) PrivateFailure(std::errc::not_a_directory);
  return created;
}

void CreateDirectories(const FsPath &path) {
  // One root-to-leaf walk, not a fresh walk for every missing component.
  if (!WalkPrivatePath(path, true).status.exists) PrivateFailure(std::errc::no_such_file_or_directory);
}

bool Remove(const FsPath &path) {
  const auto status = Status(path);
  if (!status.exists) return false;
  if (ValidatePrivateSpelling(path) == path.native().size()) {
    PrivateFailure(std::errc::operation_not_permitted);
  }
  const NativePathArgument argument(path, FileAccess::kPrivate);
#ifdef _WIN32
  if (status.directory ? RemoveDirectoryW(argument.c_str()) : DeleteFileW(argument.c_str())) return true;
  const DWORD error = GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
  PrivateFailure(std::error_code(static_cast<int>(error), std::system_category()));
#else
  if ((status.directory ? ::rmdir(argument.c_str()) : ::unlink(argument.c_str())) == 0) return true;
  const int error = errno;
  if (error == ENOENT) return false;
  PrivateFailure(std::error_code(error, std::generic_category()));
#endif
}

void VisitDirectory(const FsPath &path, const std::function<void(const FsPath &)> &visitor) {
  const auto status = Status(path);
  if (!status.exists) PrivateFailure(std::errc::no_such_file_or_directory);
  if (!status.directory) PrivateFailure(std::errc::not_a_directory);
#ifdef _WIN32
  WIN32_FIND_DATAW entry{};
  HANDLE search;
  {
    const NativePathArgument argument(path, FileAccess::kPrivate);
    std::wstring pattern(argument.c_str());
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    search = FindFirstFileW(pattern.c_str(), &entry);
  }
  if (search == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      // An empty enumeration is not evidence that a disappeared parent exists.
      if (!IsDirectory(path)) PrivateFailure(std::errc::no_such_file_or_directory);
      return;
    }
    PrivateFailure(std::error_code(static_cast<int>(error), std::system_category()));
  }
  struct SearchGuard {
    HANDLE handle;
    ~SearchGuard() { if (handle != INVALID_HANDLE_VALUE) FindClose(handle); }
  } guard{search};
  for (;;) {
    const PathView name(entry.cFileName);
    if (name != L"." && name != L"..") {
      ValidatePrivateComponent(name);
      if ((entry.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) != 0) {
        PrivateFailure(std::errc::operation_not_supported);
      }
      const auto child = path / entry.cFileName;
      const auto node = ProbePrivateNode(child);
      RequireOrdinary(node);
      if (!node.status.exists) PrivateFailure(std::errc::no_such_file_or_directory);
      visitor(child);
    }
    if (FindNextFileW(search, &entry)) continue;
    const DWORD error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) {
      PrivateFailure(std::error_code(static_cast<int>(error), std::system_category()));
    }
    break;
  }
  guard.handle = INVALID_HANDLE_VALUE;
  if (!FindClose(search)) {
    PrivateFailure(std::error_code(static_cast<int>(GetLastError()), std::system_category()));
  }
#else
  const NativePathArgument argument(path, FileAccess::kPrivate);
  const int fd = ::open(argument.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) PrivateFailure(std::error_code(errno, std::generic_category()));
  DIR *directory = ::fdopendir(fd);
  if (!directory) {
    const int error = errno;
    ::close(fd);
    PrivateFailure(std::error_code(error, std::generic_category()));
  }
  struct DirectoryGuard {
    DIR *directory;
    ~DirectoryGuard() { if (directory) ::closedir(directory); }
  } guard{directory};
  for (;;) {
    errno = 0;
    const auto *entry = ::readdir(directory);
    if (!entry) {
      if (errno != 0) PrivateFailure(std::error_code(errno, std::generic_category()));
      break;
    }
    const PathView name(entry->d_name);
    if (name == "." || name == "..") continue;
    ValidatePrivateComponent(name);
    struct stat info{};
    if (::fstatat(fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
      PrivateFailure(std::error_code(errno, std::generic_category()));
    }
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) {
      PrivateFailure(std::errc::operation_not_supported);
    }
    visitor(path / entry->d_name);
  }
  guard.directory = nullptr;
  if (::closedir(directory) != 0) PrivateFailure(std::error_code(errno, std::generic_category()));
#endif
}

bool IsEmpty(const FsPath &path) {
  const auto status = Status(path);
  if (!status.exists) PrivateFailure(std::errc::no_such_file_or_directory);
  if (status.regular) return status.size == 0;
  bool empty = true;
  VisitDirectory(path, [&](const FsPath &) { empty = false; });
  return empty;
}
}  // namespace private_io

bool GetFilesystemTimes(const std::string &utf8_path, int64_t *out_created_ms,
                        int64_t *out_modified_ms, FileAccess access) {
  if (access == FileAccess::kPrivate) {
    if (utf8_path.find('\0') != std::string::npos) return false;
    try {
      const auto path = PathFromUtf8(utf8_path);
      const auto node = WalkPrivatePath(path);
      if (!node.status.exists) return false;
#ifdef _WIN32
      const int64_t modified = FileTimeToUnixMillis(node.info.ftLastWriteTime);
      int64_t created = FileTimeToUnixMillis(node.info.ftCreationTime);
#elif defined(__APPLE__)
      const int64_t modified = static_cast<int64_t>(node.info.st_mtimespec.tv_sec) * 1000 +
                               node.info.st_mtimespec.tv_nsec / 1000000;
      int64_t created = static_cast<int64_t>(node.info.st_birthtimespec.tv_sec) * 1000 +
                        node.info.st_birthtimespec.tv_nsec / 1000000;
#else
      const int64_t modified = static_cast<int64_t>(node.info.st_mtim.tv_sec) * 1000 +
                               node.info.st_mtim.tv_nsec / 1000000;
      int64_t created = modified;
#if defined(__linux__) && defined(STATX_BTIME) && \
    (!defined(__ANDROID__) || __ANDROID_API__ >= 30)
      struct statx info{};
      const NativePathArgument argument(path, access);
      if (::statx(AT_FDCWD, argument.c_str(), AT_SYMLINK_NOFOLLOW | AT_STATX_SYNC_AS_STAT,
                  STATX_BTIME | STATX_TYPE, &info) == 0) {
        if (!S_ISREG(info.stx_mode) && !S_ISDIR(info.stx_mode)) return false;
        if (info.stx_mask & STATX_BTIME) {
          created = static_cast<int64_t>(info.stx_btime.tv_sec) * 1000 +
                    info.stx_btime.tv_nsec / 1000000;
        }
      } else if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) {
        return false;
      }
#endif
#endif
      if (created <= 0) created = modified;
      if (out_modified_ms) *out_modified_ms = modified;
      if (out_created_ms) *out_created_ms = created;
      return true;
    } catch (...) {
      return false;
    }
  }
#ifdef _WIN32
  const std::wstring wpath = PathFromUtf8(utf8_path).wstring();
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &data)) {
    return false;
  }
  const int64_t modified = FileTimeToUnixMillis(data.ftLastWriteTime);
  int64_t created = FileTimeToUnixMillis(data.ftCreationTime);
  if (created <= 0) created = modified;
  if (out_modified_ms) *out_modified_ms = modified;
  if (out_created_ms) *out_created_ms = created;
  return true;
#elif defined(__APPLE__)
  struct stat st;
  if (::stat(utf8_path.c_str(), &st) != 0) return false;
  const int64_t modified =
      static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000 + st.st_mtimespec.tv_nsec / 1000000;
  int64_t created = static_cast<int64_t>(st.st_birthtimespec.tv_sec) * 1000 +
                    st.st_birthtimespec.tv_nsec / 1000000;
  if (created <= 0) created = modified;
  if (out_modified_ms) *out_modified_ms = modified;
  if (out_created_ms) *out_created_ms = created;
  return true;
#else
  // Linux / other POSIX: prefer statx for birth time; fall back to stat.
#if defined(__linux__) && defined(STATX_BTIME) && \
    (!defined(__ANDROID__) || __ANDROID_API__ >= 30)
  struct statx stx;
  if (::statx(AT_FDCWD, utf8_path.c_str(), AT_STATX_SYNC_AS_STAT, STATX_BTIME | STATX_MTIME,
              &stx) == 0) {
    const int64_t modified =
        static_cast<int64_t>(stx.stx_mtime.tv_sec) * 1000 + stx.stx_mtime.tv_nsec / 1000000;
    int64_t created = modified;
    if (stx.stx_mask & STATX_BTIME) {
      created =
          static_cast<int64_t>(stx.stx_btime.tv_sec) * 1000 + stx.stx_btime.tv_nsec / 1000000;
    }
    if (created <= 0) created = modified;  // birth time absent/zero -> fall back to modified
    if (out_modified_ms) *out_modified_ms = modified;
    if (out_created_ms) *out_created_ms = created;
    return true;
  }
#endif
  struct stat st;
  if (::stat(utf8_path.c_str(), &st) != 0) return false;
  const int64_t modified = static_cast<int64_t>(st.st_mtime) * 1000;
  if (out_modified_ms) *out_modified_ms = modified;
  if (out_created_ms) *out_created_ms = modified;  // no portable birth time
  return true;
#endif
}

std::pair<std::string, std::string> SplitPath(const std::string &path) {
  size_t last_slash = path.find_last_of(kPathSeparator);
  if (last_slash == std::string::npos) {
    return {".", path};
  } else {
    std::string parent_path = path.substr(0, last_slash);
    std::string child_name = path.substr(last_slash + 1);
    return {parent_path, child_name};
  }
}

std::vector<std::string> SplitPathComponents(const std::string &path) {
  std::vector<std::string> components;
  size_t start = 0;
  size_t end = path.find(kPathSeparator);
  while (end != std::string::npos) {
    if (end != start) {
      components.push_back(path.substr(start, end - start));
    }
    start = end + 1;
    end = path.find(kPathSeparator, start);
  }
  if (start < path.size()) {
    components.push_back(path.substr(start));
  }
  return components;
}

namespace {

ReparseState CheckReparsePointFs(const std::filesystem::path &path) {
  if (path.empty()) {
    return ReparseState::kError;
  }
#ifdef _WIN32
  const DWORD attrs = GetFileAttributesW(path.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    return ReparseState::kError;
  }
  return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ? ReparseState::kYes : ReparseState::kNo;
#else
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec) {
    return ReparseState::kError;
  }
  return std::filesystem::is_symlink(status) ? ReparseState::kYes : ReparseState::kNo;
#endif
}

// Component-wise prefix test. |canonical_root| is a prefix of |canonical_path|
// when the root iterator is exhausted first. Equal paths count as "within".
bool IsPrefixPath(const std::filesystem::path &canonical_root,
                  const std::filesystem::path &canonical_path) {
  auto mismatch_pair = std::mismatch(canonical_root.begin(), canonical_root.end(),
                                     canonical_path.begin(), canonical_path.end());
  return mismatch_pair.first == canonical_root.end();
}

}  // namespace

ReparseState CheckReparsePoint(const std::string &path, FileAccess access) {
  if (access == FileAccess::kDefault) return CheckReparsePointFs(PathFromUtf8(path));
  if (path.find('\0') != std::string::npos) return ReparseState::kError;
  try {
    const auto node = WalkPrivatePath(PathFromUtf8(path), false, true);
    if (!node.status.exists) return ReparseState::kError;
    return node.reparse ? ReparseState::kYes : ReparseState::kNo;
  } catch (...) {
    return ReparseState::kError;
  }
}

bool IsReparsePoint(const std::string &path, bool on_error) {
  switch (CheckReparsePoint(path)) {
    case ReparseState::kYes:
      return true;
    case ReparseState::kNo:
      return false;
    case ReparseState::kError:
    default:
      return on_error;
  }
}

bool FileIdentity::SameObject(const FileIdentity &other) const noexcept {
  return device != 0 && (low != 0 || high != 0) && device == other.device &&
         low == other.low && high == other.high && directory == other.directory;
}

VxCoreError GetFileIdentity(const std::filesystem::path &path, FileIdentity &out,
                            FileAccess access) noexcept {
  out = {};
  if (access == FileAccess::kPrivate) {
    try {
      out = PrivateIdentity(WalkPrivatePath(path, false, false, true));
      return VXCORE_OK;
    } catch (const std::bad_alloc &) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    } catch (const std::filesystem::filesystem_error &error) {
      return PrivateMappedError(error.code());
    } catch (...) {
      return VXCORE_ERR_IO;
    }
  }
  if (path.empty() || path.native().find(std::filesystem::path::value_type{}) !=
                          std::filesystem::path::string_type::npos) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  // lstat("link/") follows the link on POSIX. Probe the actual final entry,
  // preserving filesystem roots while removing an optional trailing separator.
  try {
    if (!path.has_filename() && path.has_relative_path()) {
      return GetFileIdentity(path.parent_path(), out, access);
    }
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
  FileIdentity identity;
#ifdef _WIN32
  HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) return VXCORE_ERR_IO;
  BY_HANDLE_FILE_INFORMATION info{};
  FILE_ID_INFO id{};
  const bool valid = GetFileType(handle) == FILE_TYPE_DISK &&
                     GetFileInformationByHandle(handle, &info) &&
                     (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) == 0 &&
                     GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id));
  const bool closed = CloseHandle(handle) != 0;
  if (!valid || !closed) return VXCORE_ERR_IO;
  identity.device = id.VolumeSerialNumber;
  std::memcpy(&identity.low, id.FileId.Identifier, sizeof(identity.low));
  std::memcpy(&identity.high, id.FileId.Identifier + sizeof(identity.low), sizeof(identity.high));
  identity.links = info.nNumberOfLinks;
  identity.directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0 || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))) {
    return VXCORE_ERR_IO;
  }
  identity.device = static_cast<uint64_t>(info.st_dev);
  identity.low = static_cast<uint64_t>(info.st_ino);
  identity.links = static_cast<uint64_t>(info.st_nlink);
  identity.directory = S_ISDIR(info.st_mode);
#endif
  if (identity.device == 0 || (identity.low == 0 && identity.high == 0) || identity.links == 0) {
    return VXCORE_ERR_IO;
  }
  out = identity;
  return VXCORE_OK;
}

VxCoreError RenameNoReplace(const std::filesystem::path &from, const std::filesystem::path &to,
                           FileAccess access) {
  if (from.empty() || to.empty() ||
      from.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos ||
      to.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos) {
#ifdef _WIN32
    SetLastError(ERROR_INVALID_PARAMETER);
#endif
    return VXCORE_ERR_INVALID_PARAM;
  }
  try {
    if (access == FileAccess::kPrivate) {
      private_io::Validate(from);
      private_io::Validate(to, true);
    }
    const NativePathArgument source(from, access), destination(to, access);
#ifdef _WIN32
  if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) return VXCORE_OK;
  const DWORD error = GetLastError();
  return error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY
             ? VXCORE_ERR_OUT_OF_MEMORY : VXCORE_ERR_IO;
#elif defined(__linux__)
  // Bionic's renameat2 wrapper requires API30; the raw syscall works at minAPI28.
#ifdef SYS_renameat2
  if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) == 0) {
    return VXCORE_OK;
  }
  return errno == ENOMEM ? VXCORE_ERR_OUT_OF_MEMORY : VXCORE_ERR_IO;
#else
  return VXCORE_ERR_UNSUPPORTED;
#endif
#elif defined(__APPLE__)
  if (::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0) return VXCORE_OK;
  return errno == ENOMEM ? VXCORE_ERR_OUT_OF_MEMORY : VXCORE_ERR_IO;
#else
  return VXCORE_ERR_UNSUPPORTED;
#endif
  } catch (const std::bad_alloc &) {
#ifdef _WIN32
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
#endif
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::filesystem::filesystem_error &error) {
    return PrivateMappedError(error.code());
  } catch (...) {
#ifdef _WIN32
    SetLastError(ERROR_INVALID_DATA);
#endif
    return VXCORE_ERR_IO;
  }
}

VxCoreError FlushDirectory(const std::filesystem::path &path, FileAccess access) {
#ifdef _WIN32
  // Windows does not provide the POSIX directory-fsync contract. Check the
  // directory, relying on the writer's file flush and WRITE_THROUGH publication.
  FileIdentity identity;
  const auto error = GetFileIdentity(path, identity, access);
  return error != VXCORE_OK ? error : identity.directory ? VXCORE_OK : VXCORE_ERR_IO;
#else
  try {
  if (access == FileAccess::kPrivate && !private_io::IsDirectory(path)) return VXCORE_ERR_IO;
  if (path.empty() || path.native().find('\0') != std::string::npos) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  if (!path.has_filename() && path.has_relative_path()) {
    try {
      return FlushDirectory(path.parent_path(), access);
    } catch (const std::bad_alloc &) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    } catch (...) {
      return VXCORE_ERR_IO;
    }
  }
  const NativePathArgument argument(path, access);
  const int fd = ::open(argument.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return errno == ENOMEM ? VXCORE_ERR_OUT_OF_MEMORY : VXCORE_ERR_IO;
  const int flushed = ::fsync(fd);
  const int flush_error = errno;
  const int closed = ::close(fd);
  if (flushed != 0) return flush_error == ENOMEM ? VXCORE_ERR_OUT_OF_MEMORY : VXCORE_ERR_IO;
  return closed == 0 ? VXCORE_OK : VXCORE_ERR_IO;
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::filesystem::filesystem_error &error) {
    return PrivateMappedError(error.code());
  } catch (...) {
    return VXCORE_ERR_IO;
  }
#endif
}

bool IsPathWithinCanonical(const std::filesystem::path &canonical_root, const std::string &path,
                           bool on_error) {
  if (canonical_root.empty() || path.empty()) {
    return on_error;
  }

  // PathFromUtf8 yields an empty path when the input is not valid UTF-8.
  const std::filesystem::path input_path = PathFromUtf8(path);
  if (input_path.empty()) {
    return on_error;
  }

  std::error_code path_ec;
  const std::filesystem::path canonical_path =
      std::filesystem::weakly_canonical(input_path, path_ec);
  if (path_ec) {
    return on_error;
  }

  return IsPrefixPath(canonical_root, canonical_path);
}

bool IsPathWithin(const std::string &root, const std::string &path, bool on_error) {
  if (root.empty()) {
    return on_error;
  }

  const std::filesystem::path root_path = PathFromUtf8(root);
  if (root_path.empty()) {
    return on_error;
  }

  std::error_code root_ec;
  const std::filesystem::path canonical_root =
      std::filesystem::weakly_canonical(root_path, root_ec);
  if (root_ec) {
    return on_error;
  }

  return IsPathWithinCanonical(canonical_root, path, on_error);
}

namespace {

bool CopyTreeSkipReparsePointsImpl(const std::filesystem::path &src,
                                   const std::filesystem::path &dest,
                                   const std::filesystem::path &canonical_src_root) {
  namespace fs = std::filesystem;

  std::error_code ec;
  fs::create_directories(dest, ec);
  if (ec) {
    std::error_code dir_ec;
    if (!fs::is_directory(dest, dir_ec) || dir_ec) {
      VXCORE_LOG_WARN("CopyTree: Failed to create directory %s: %s", PathToUtf8(dest).c_str(),
                      ec.message().c_str());
      return false;
    }
  }

  // NOTE: skip_permission_denied is deliberately NOT used. A source entry that
  // cannot be read is an IO error, not a skip: reporting success after silently
  // omitting data would turn a truncated copy into a VXCORE_OK.
  ec.clear();
  fs::directory_iterator it(src, ec);
  if (ec) {
    VXCORE_LOG_WARN("CopyTree: Failed to enumerate %s: %s", PathToUtf8(src).c_str(),
                    ec.message().c_str());
    return false;
  }

  // Pass 1: collect the entries. Iterating and mutating in one pass risks
  // iterator invalidation, and collecting first keeps the increment-error check
  // to a single site: increment(ec) can BOTH set `ec` and turn the iterator
  // into the end iterator, so an error checked only at the top of a loop body
  // would be missed on the final increment and a truncated copy would be
  // reported as success.
  std::vector<std::filesystem::path> entries;
  while (it != fs::directory_iterator()) {
    entries.push_back(it->path());
    it.increment(ec);
    if (ec) {
      VXCORE_LOG_WARN("CopyTree: Enumeration error under %s: %s", PathToUtf8(src).c_str(),
                      ec.message().c_str());
      return false;
    }
  }

  // Pass 2: copy.
  for (const fs::path &entry_path : entries) {
    const std::string entry_utf8 = PathToUtf8(entry_path);

    // Primary defense: never follow symlinks / junctions / other reparse points.
    const ReparseState reparse_state = CheckReparsePointFs(entry_path);
    if (reparse_state == ReparseState::kError) {
      VXCORE_LOG_WARN("CopyTree: Failed to stat entry: %s", entry_utf8.c_str());
      return false;
    }
    if (reparse_state == ReparseState::kYes) {
      VXCORE_LOG_WARN("CopyTree: Skipping symlink/junction/reparse point: %s", entry_utf8.c_str());
      continue;
    }

    const fs::path entry_dest = dest / entry_path.filename();

    std::error_code stat_ec;
    const bool is_dir = fs::is_directory(entry_path, stat_ec);
    if (stat_ec) {
      VXCORE_LOG_WARN("CopyTree: Failed to stat %s: %s", entry_utf8.c_str(),
                      stat_ec.message().c_str());
      return false;
    }
    if (is_dir) {
      // Defense in depth: cannot verify containment -> treat as outside -> skip.
      if (!IsPathWithinCanonical(canonical_src_root, entry_utf8, /*on_error=*/false)) {
        VXCORE_LOG_WARN("CopyTree: Skipping subdirectory outside source root: %s",
                        entry_utf8.c_str());
        continue;
      }
      if (!CopyTreeSkipReparsePointsImpl(entry_path, entry_dest, canonical_src_root)) {
        return false;
      }
      continue;
    }

    stat_ec.clear();
    const bool is_file = fs::is_regular_file(entry_path, stat_ec);
    if (stat_ec) {
      VXCORE_LOG_WARN("CopyTree: Failed to stat %s: %s", entry_utf8.c_str(),
                      stat_ec.message().c_str());
      return false;
    }
    if (is_file) {
      std::error_code copy_ec;
      fs::copy_file(entry_path, entry_dest, fs::copy_options::overwrite_existing, copy_ec);
      if (copy_ec) {
        VXCORE_LOG_WARN("CopyTree: Failed to copy %s: %s", entry_utf8.c_str(),
                        copy_ec.message().c_str());
        return false;
      }
    }
    // Anything else (fifo, socket, block device, ...) is not copyable content
    // and is skipped, matching the previous fs::copy behaviour.
  }

  return true;
}

}  // namespace

bool CopyTreeSkipReparsePoints(const std::filesystem::path &src,
                               const std::filesystem::path &dest) {
  // Hard exception boundary: callers map `false` to VXCORE_ERR_IO, which is the
  // code the previous try/catch around fs::copy returned. Letting an exception
  // escape here would surface as VXCORE_ERR_UNKNOWN from the C API instead.
  try {
    if (CheckReparsePointFs(src) != ReparseState::kNo) {
      VXCORE_LOG_WARN("CopyTree: Source is a symlink/junction/reparse point or is unreadable: %s",
                      PathToUtf8(src).c_str());
      return false;
    }

    std::error_code ec;
    const std::filesystem::path canonical_src = std::filesystem::weakly_canonical(src, ec);
    if (ec) {
      VXCORE_LOG_WARN("CopyTree: Failed to canonicalize source %s: %s", PathToUtf8(src).c_str(),
                      ec.message().c_str());
      return false;
    }

    return CopyTreeSkipReparsePointsImpl(src, dest, canonical_src);
  } catch (const std::exception &e) {
    VXCORE_LOG_WARN("CopyTree: Aborted with exception: %s", e.what());
    return false;
  } catch (...) {
    VXCORE_LOG_WARN("CopyTree: Aborted with unknown exception");
    return false;
  }
}

bool IsRelativePath(const std::string &path) { return PathFromUtf8(path).is_relative(); }

bool IsSingleName(const std::string &path) {
  if (path.empty()) {
    return false;
  }
  if (!PathFromUtf8(path).is_relative()) {
    return false;
  }
  return path.find(kPathSeparator) == std::string::npos && path.find('\\') == std::string::npos;
}

std::string RelativePath(const std::string &base, const std::string &path) {
  if (base.empty()) {
    return std::string();
  }

  if (path.empty()) {
    return ".";
  }

  if (path.size() < base.size() || path.substr(0, base.size()) != base) {
    return std::string();
  }

  if (path.size() > base.size() && path[base.size()] == kPathSeparator) {
    return path.substr(base.size() + 1);
  } else {
    return path.substr(base.size());
  }
}

VxCoreError ReadFile(const std::filesystem::path &path, std::string &out_content) {
  try {
    std::ifstream file(path);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    out_content = ss.str();
    return VXCORE_OK;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

VxCoreError ReadFileHead(const std::filesystem::path &path, size_t max_bytes,
                         std::string &out_content) {
  try {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }
    out_content.resize(max_bytes);
    file.read(&out_content[0], max_bytes);
    out_content.resize(static_cast<size_t>(file.gcount()));
    return VXCORE_OK;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

VxCoreError WriteFile(const std::filesystem::path &path, const std::string &content) {
  try {
    std::ofstream file(path);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }
    file << content;
    return VXCORE_OK;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

namespace {

VxCoreError AtomicWriteError() {
  return errno == ENOMEM ? VXCORE_ERR_OUT_OF_MEMORY : VXCORE_ERR_IO;
}

void ValidatePrivatePublication(const FsPath &temporary, const FileIdentity &owned,
                                const FsPath &destination) {
  FileIdentity current;
  const auto error = GetFileIdentity(temporary, current, FileAccess::kPrivate);
  if (error != VXCORE_OK) {
    PrivateFailure(error == VXCORE_ERR_OUT_OF_MEMORY ? std::errc::not_enough_memory : std::errc::io_error);
  }
  if (!owned.SameObject(current) || current.directory || current.links != 1) {
    PrivateFailure(std::errc::io_error);
  }
  const auto status = private_io::Status(destination);
  if (status.exists && !status.regular) PrivateFailure(std::errc::is_a_directory);
}

// Match the bounded sharing-violation retry policy used by folder-config publication.
constexpr int kAtomicRenameRetries = 5;
constexpr int kAtomicRenameRetryDelayMs = 40;
constexpr int kAtomicTempAttempts = 64;

}  // namespace

AtomicFileWriter::AtomicFileWriter(const std::filesystem::path &path, FileAccess access)
    : access_(access) {
  try {
    path_ = path;
  } catch (const std::bad_alloc &) {
    error_ = VXCORE_ERR_OUT_OF_MEMORY;
  } catch (...) {
    error_ = VXCORE_ERR_IO;
  }
}

AtomicFileWriter::~AtomicFileWriter() {
  if (file_) {
    std::fclose(file_);
  }
  if (owns_temp_ && access_ == FileAccess::kPrivate) {
    try {
      FileIdentity current;
      if (GetFileIdentity(temp_path_, current, access_) == VXCORE_OK &&
          temp_identity_.SameObject(current) && current.links == 1) {
        private_io::Remove(temp_path_);
      }
    } catch (...) {
      // A replaced/unverifiable object is not owned cleanup authority.
    }
  } else if (owns_temp_) {
#ifdef _WIN32
    _wremove(temp_path_.c_str());
#else
    ::unlink(temp_path_.c_str());
#endif
  }
}

VxCoreError AtomicFileWriter::Open() {
  if (error_ != VXCORE_OK) {
    return error_;
  }
  if (opened_) {
    return VXCORE_ERR_INVALID_STATE;
  }
  opened_ = true;

  try {
    if (path_.empty() || !path_.has_filename() ||
        path_.native().find(std::filesystem::path::value_type{}) !=
            std::filesystem::path::string_type::npos) {
      return error_ = VXCORE_ERR_INVALID_PARAM;
    }

    if (access_ == FileAccess::kPrivate) {
      const auto destination = private_io::Status(path_);
      if ((destination.exists && !destination.regular) ||
          !private_io::IsDirectory(path_.parent_path())) return error_ = VXCORE_ERR_IO;
    }

    // Exclusivity, not name unpredictability, prevents reuse of another writer's
    // file or a pre-existing symlink. No UUID/crypto dependency is needed here.
    static std::atomic<uint64_t> sequence{0};
#ifdef _WIN32
    const auto process_id = GetCurrentProcessId();
#else
    const auto process_id = ::getpid();
#endif
    for (int attempt = 0; attempt < kAtomicTempAttempts; ++attempt) {
      temp_path_ = path_;
      temp_path_ += PathFromUtf8(".tmp-" + std::to_string(process_id) + "-" +
                                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
#ifdef _WIN32
      if (access_ == FileAccess::kDefault) {
        // The destination can fit MAX_PATH while our exclusive temporary sibling does not.
        // Extend only the internal temporary path; preserve normal destination semantics.
        const auto absolute_temp =
            std::filesystem::absolute(temp_path_).lexically_normal().make_preferred();
        const auto &native_temp = absolute_temp.native();
        if (native_temp.size() >= MAX_PATH && native_temp.compare(0, 4, L"\\\\?\\") != 0) {
          temp_path_ = native_temp.compare(0, 2, L"\\\\") == 0
                           ? std::filesystem::path(L"\\\\?\\UNC\\" + native_temp.substr(2))
                           : std::filesystem::path(L"\\\\?\\" + native_temp);
        }
      }
      const NativePathArgument argument(temp_path_, access_);
      const int fd = _wopen(argument.c_str(),
                           _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                           _S_IREAD | _S_IWRITE);
#else
      const NativePathArgument argument(temp_path_, access_);
      const int fd = ::open(argument.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                            (access_ == FileAccess::kPrivate ? O_NOFOLLOW : 0),
                            S_IRUSR | S_IWUSR);
#endif
      if (fd < 0) {
        if (errno == EEXIST) {
          continue;
        }
        return error_ = AtomicWriteError();
      }

      owns_temp_ = true;
      if (access_ == FileAccess::kPrivate) {
        try {
          temp_identity_ = PrivateOpenFileIdentity(fd);
        } catch (...) {
#ifdef _WIN32
          _close(fd);
#else
          ::close(fd);
#endif
          throw;
        }
      }
#ifdef _WIN32
      file_ = _wfdopen(fd, L"wb");
#else
      file_ = ::fdopen(fd, "wb");
#endif
      if (!file_) {
        error_ = AtomicWriteError();
#ifdef _WIN32
        _close(fd);
#else
        ::close(fd);
#endif
        return error_;
      }
      return VXCORE_OK;
    }
    return error_ = VXCORE_ERR_IO;
  } catch (const std::bad_alloc &) {
    return error_ = VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::filesystem::filesystem_error &error) {
    return error_ = access_ == FileAccess::kPrivate ? PrivateMappedError(error.code()) : VXCORE_ERR_IO;
  } catch (...) {
    return error_ = VXCORE_ERR_IO;
  }
}

VxCoreError AtomicFileWriter::Write(const void *data, size_t size) {
  if (error_ != VXCORE_OK) {
    return error_;
  }
  if (!file_) {
    return VXCORE_ERR_INVALID_STATE;
  }
  if (!data && size != 0) {
    return error_ = VXCORE_ERR_NULL_POINTER;
  }
  errno = 0;
  if (size != 0 && std::fwrite(data, 1, size, file_) != size) {
    return error_ = AtomicWriteError();
  }
  return VXCORE_OK;
}

VxCoreError AtomicFileWriter::Commit() {
  if (error_ != VXCORE_OK) {
    return error_;
  }
  if (!file_) {
    return VXCORE_ERR_INVALID_STATE;
  }

  errno = 0;
  if (std::fflush(file_) != 0) {
    error_ = AtomicWriteError();
  }
  if (error_ == VXCORE_OK) {
    errno = 0;
#ifdef _WIN32
    if (_commit(_fileno(file_)) != 0) {
#else
    if (::fsync(fileno(file_)) != 0) {
#endif
      error_ = AtomicWriteError();
    }
  }
  errno = 0;
  const int close_result = std::fclose(file_);
  file_ = nullptr;
  if (close_result != 0 && error_ == VXCORE_OK) {
    error_ = AtomicWriteError();
  }
  if (error_ != VXCORE_OK) {
    return error_;
  }

  try {
    for (int attempt = 0; attempt < kAtomicRenameRetries; ++attempt) {
      if (access_ == FileAccess::kPrivate) {
        ValidatePrivatePublication(temp_path_, temp_identity_, path_);
      }
      std::error_code ec;
      // Same-directory rename replaces the destination atomically, without an
      // intermediate removal that could lose the previous live file on failure.
#ifdef _WIN32
      const NativePathArgument source(temp_path_, access_), destination(path_, access_);
      if (!MoveFileExW(source.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
      }
#else
      if (access_ == FileAccess::kPrivate) {
        const NativePathArgument source(temp_path_, access_), destination(path_, access_);
        if (::rename(source.c_str(), destination.c_str()) != 0) {
          ec = std::error_code(errno, std::generic_category());
        }
      } else {
        std::filesystem::rename(temp_path_, path_, ec);
      }
#endif
      if (!ec) {
        owns_temp_ = false;
        return error_ = access_ == FileAccess::kPrivate ? FlushDirectory(path_.parent_path(), access_)
                                                       : VXCORE_OK;
      }
      if (ec == std::errc::not_enough_memory) {
        return error_ = VXCORE_ERR_OUT_OF_MEMORY;
      }
      if (attempt + 1 < kAtomicRenameRetries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kAtomicRenameRetryDelayMs));
      }
    }
    return error_ = VXCORE_ERR_IO;
  } catch (const std::bad_alloc &) {
    return error_ = VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::filesystem::filesystem_error &error) {
    return error_ = access_ == FileAccess::kPrivate ? PrivateMappedError(error.code()) : VXCORE_ERR_IO;
  } catch (...) {
    return error_ = VXCORE_ERR_IO;
  }
}

VxCoreError AtomicFileWriter::CommitNoReplace() {
  if (error_ != VXCORE_OK) return error_;
  if (!file_) return VXCORE_ERR_INVALID_STATE;

  errno = 0;
  if (std::fflush(file_) != 0) error_ = AtomicWriteError();
  if (error_ == VXCORE_OK) {
    errno = 0;
#ifdef _WIN32
    if (_commit(_fileno(file_)) != 0) {
#else
    if (::fsync(fileno(file_)) != 0) {
#endif
      error_ = AtomicWriteError();
    }
  }
  errno = 0;
  const int closed = std::fclose(file_);
  file_ = nullptr;
  if (closed != 0 && error_ == VXCORE_OK) error_ = AtomicWriteError();
  if (error_ != VXCORE_OK) return error_;

  try {
    const auto parent = path_.has_parent_path() ? path_.parent_path() : std::filesystem::path(".");
#ifdef _WIN32
    for (int attempt = 0; attempt < kAtomicRenameRetries; ++attempt) {
      if (access_ == FileAccess::kPrivate) {
        ValidatePrivatePublication(temp_path_, temp_identity_, path_);
      }
      error_ = RenameNoReplace(temp_path_, path_, access_);
      if (error_ == VXCORE_OK) break;
      const DWORD native_error = GetLastError();
      if (native_error != ERROR_SHARING_VIOLATION || attempt + 1 == kAtomicRenameRetries) {
        return error_;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kAtomicRenameRetryDelayMs));
    }
#else
    if (access_ == FileAccess::kPrivate) {
      ValidatePrivatePublication(temp_path_, temp_identity_, path_);
    }
    error_ = RenameNoReplace(temp_path_, path_, access_);
    if (error_ != VXCORE_OK) return error_;
#endif
    // Publication has happened. Even a failed parent flush cannot authorize
    // destruction (or an error-handling caller) to remove the complete file.
    owns_temp_ = false;
    return error_ = FlushDirectory(parent, access_);
  } catch (const std::bad_alloc &) {
    return error_ = VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::filesystem::filesystem_error &error) {
    return error_ = access_ == FileAccess::kPrivate ? PrivateMappedError(error.code()) : VXCORE_ERR_IO;
  } catch (...) {
    return error_ = VXCORE_ERR_IO;
  }
}

VxCoreError WriteFileAtomic(const std::filesystem::path &path, const std::string &content,
                            FileAccess access) {
  AtomicFileWriter writer(path, access);
  VxCoreError error = writer.Open();
  if (error == VXCORE_OK) {
    error = writer.Write(content.data(), content.size());
  }
  return error == VXCORE_OK ? writer.Commit() : error;
}

VxCoreError LoadJsonFile(const std::filesystem::path &path, nlohmann::json &out_json) {
  try {
    std::ifstream file(path);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }
    file >> out_json;
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

}  // namespace vxcore
