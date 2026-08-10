#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "file_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#if defined(VXCORE_BUILD_DLL)
#include "logger.h"
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
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

bool GetFilesystemTimes(const std::string &utf8_path, int64_t *out_created_ms,
                        int64_t *out_modified_ms) {
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
#if defined(__linux__) && defined(STATX_BTIME)
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

ReparseState CheckReparsePoint(const std::string &path) {
  return CheckReparsePointFs(PathFromUtf8(path));
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
