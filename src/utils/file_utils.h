#ifndef VXCORE_FILE_UTILS_H
#define VXCORE_FILE_UTILS_H

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "vxcore/vxcore_types.h"

namespace vxcore {

// Only validated private storage opts in. Logical paths always remain unprefixed.
enum class FileAccess { kDefault, kPrivate };

// A final OS/CRT argument, not a filesystem path. The default/POSIX argument
// borrows its logical input, which must outlive it. Private Windows spelling is
// owned and never passed through path normalization or used as stored authority.
class NativePathArgument final {
 public:
  explicit NativePathArgument(const std::filesystem::path &path,
                              FileAccess access = FileAccess::kDefault);
  const std::filesystem::path::value_type *c_str() const noexcept;

 private:
  const std::filesystem::path::value_type *borrowed_ = nullptr;
#ifdef _WIN32
  std::wstring owned_;
#endif
};

namespace private_io {
struct PathStatus {
  bool exists = false, directory = false, regular = false;
  uint64_t size = 0, modified = 0;
};
// Clean absolute logical spelling; no-follow checks from the physical root.
// Only true not-found is absence. Errors never include the logical path.
PathStatus Status(const std::filesystem::path &path);
bool Exists(const std::filesystem::path &path);
bool IsDirectory(const std::filesystem::path &path);
bool IsRegularFile(const std::filesystem::path &path);
bool IsEmpty(const std::filesystem::path &path);
uint64_t FileSize(const std::filesystem::path &path);
uint64_t ModifiedTime(const std::filesystem::path &path);  // exact comparison token
bool MakeDirectory(const std::filesystem::path &path);
void CreateDirectories(const std::filesystem::path &path);
bool Remove(const std::filesystem::path &path);  // leaf/empty directory, never recursive
void VisitDirectory(const std::filesystem::path &path,
                    const std::function<void(const std::filesystem::path &)> &visitor);
void Validate(const std::filesystem::path &path, bool allow_missing = false);
}  // namespace private_io

std::filesystem::path PathFromUtf8(const std::string &utf8_str);

std::string PathToUtf8(const std::filesystem::path &path);

std::string PathToGenericUtf8(const std::filesystem::path &path);

inline std::string CleanFsPath(const std::filesystem::path &path) {
  if (path.empty()) {
    return ".";
  }
  return PathToGenericUtf8(path.lexically_normal());
}

inline std::string CleanPath(const std::string &path) {
  if (path.empty()) {
    return ".";
  }
  return CleanFsPath(PathFromUtf8(path));
}

// UTF-8-safe filesystem query wrappers.
// Use these instead of calling std::filesystem functions with raw std::string paths.
inline bool PathExists(const std::string &utf8_path) {
  if (utf8_path.empty()) {
    return false;
  }
  return std::filesystem::exists(PathFromUtf8(utf8_path));
}

inline bool IsDirectory(const std::string &utf8_path) {
  if (utf8_path.empty()) {
    return false;
  }
  return std::filesystem::is_directory(PathFromUtf8(utf8_path));
}

inline bool IsRegularFile(const std::string &utf8_path) {
  if (utf8_path.empty()) {
    return false;
  }
  return std::filesystem::is_regular_file(PathFromUtf8(utf8_path));
}

// Extract filename from a UTF-8 path string safely.
inline std::string PathFilename(const std::string &utf8_path) {
  if (utf8_path.empty()) {
    return {};
  }
  return PathToUtf8(PathFromUtf8(utf8_path).filename());
}

// Result of a reparse-point probe. "Could not be stat'ed" is deliberately a
// distinct state from "is not a reparse point": collapsing them would either
// follow an unverifiable entry or silently drop a readable one.
enum class ReparseState { kNo, kYes, kError };

// Tri-state reparse-point probe. Never follows the link. Prefer this over
// IsReparsePoint when the caller needs to distinguish a stat failure (an IO
// error worth reporting) from a confirmed link (a skip).
ReparseState CheckReparsePoint(const std::string &path,
                               FileAccess access = FileAccess::kDefault);

// True if |path| is a reparse point.
// Windows: any FILE_ATTRIBUTE_REPARSE_POINT, which covers symlinks, directory
//          junctions (IO_REPARSE_TAG_MOUNT_POINT) and other redirecting or
//          virtualizing tags. std::filesystem::is_symlink() is NOT sufficient
//          here: MSVC's STL maps junctions to the implementation-defined
//          file_type::junction, which is_symlink() does not report.
// POSIX:   symlink_status().type() == file_type::symlink.
// Never follows the link. Returns |on_error| if the path cannot be stat'ed.
bool IsReparsePoint(const std::string &path, bool on_error);

// No-follow identity of an ordinary file or directory. Parent traversal remains
// the caller's responsibility. Link counts may change without changing identity.
struct FileIdentity {
  uint64_t device = 0, low = 0, high = 0, links = 0;
  bool directory = false;
  bool SameObject(const FileIdentity &other) const noexcept;
};

// Resets the output on failure; rejects reparse points, symlinks and special files.
VxCoreError GetFileIdentity(const std::filesystem::path &path, FileIdentity &out,
                            FileAccess access = FileAccess::kDefault) noexcept;

// One same-volume atomic attempt; never replaces a destination or copies bytes.
// Callers own any bounded Windows sharing-violation retry policy.
VxCoreError RenameNoReplace(const std::filesystem::path &from, const std::filesystem::path &to,
                           FileAccess access = FileAccess::kDefault);

// POSIX checks directory fsync and close. Windows only verifies an ordinary
// directory: durability relies on checked file flushes and WRITE_THROUGH moves,
// not an unsupported directory-fsync/power-loss guarantee.
VxCoreError FlushDirectory(const std::filesystem::path &path,
                           FileAccess access = FileAccess::kDefault);

// True if |path| is |root| itself or lies under it.
// BOTH sides are canonicalized before comparing. Canonicalizing only one side
// makes the check silently pass whenever the two spellings of the same location
// differ - e.g. on Windows where a root was opened through an 8.3 short path
// (C:/Users/RUNNER~1/...) while the other side resolves to the long form
// (C:/Users/runneradmin/...), or anywhere a symlink is in play.
// Returns |on_error| if either side fails to canonicalize.
bool IsPathWithin(const std::string &root, const std::string &path, bool on_error);

// As IsPathWithin, but for a root that the caller has ALREADY canonicalized.
// Use this inside a directory walk: it canonicalizes only the candidate, so the
// root is not re-resolved (an O(depth) stat of every component) once per entry.
bool IsPathWithinCanonical(const std::filesystem::path &canonical_root, const std::string &path,
                           bool on_error);

// Recursively copy |src| to |dest|, skipping any entry that is a reparse point
// and any subdirectory that does not resolve to a location under |src|.
// Returns false on any IO error, including an entry that cannot be stat'ed or
// a directory that cannot be enumerated: a truncated copy is reported as a
// failure, never as success. This function is a hard exception boundary - no
// exceptions escape, so callers can map false onto their IO error code.
//
// TOCTOU: this is a path-based check-then-recurse guard. A source tree mutated
// concurrently by an attacker can still race between the check and the copy.
// Closing that would need handle-relative / no-follow traversal, which is not
// attempted here.
bool CopyTreeSkipReparsePoints(const std::filesystem::path &src, const std::filesystem::path &dest);

bool IsRelativePath(const std::string &path);

// Check if path is a single name (no path separators, not absolute).
bool IsSingleName(const std::string &path);

VxCoreError ReadFile(const std::filesystem::path &path, std::string &out_content);

VxCoreError WriteFile(const std::filesystem::path &path, const std::string &content);

// Single-use binary writer. The parent directory must already exist. Write failures
// prevent publication; destruction closes and removes an uncommitted temporary sibling.
class AtomicFileWriter final {
 public:
  explicit AtomicFileWriter(const std::filesystem::path &path,
                            FileAccess access = FileAccess::kDefault);
  ~AtomicFileWriter();

  AtomicFileWriter(const AtomicFileWriter &) = delete;
  AtomicFileWriter &operator=(const AtomicFileWriter &) = delete;

  VxCoreError Open();
  VxCoreError Write(const void *data, size_t size);
  VxCoreError Commit();
  // Publishes only to an absent destination. A post-publication directory-flush
  // error retains the complete destination for reconciliation, including on destruction.
  VxCoreError CommitNoReplace();

 private:
  std::filesystem::path path_;
  std::filesystem::path temp_path_;
  FileAccess access_ = FileAccess::kDefault;
  FileIdentity temp_identity_;
  std::FILE *file_ = nullptr;
  VxCoreError error_ = VXCORE_OK;
  bool opened_ = false;
  bool owns_temp_ = false;
};

VxCoreError WriteFileAtomic(const std::filesystem::path &path, const std::string &content,
                            FileAccess access = FileAccess::kDefault);

VxCoreError LoadJsonFile(const std::filesystem::path &path, nlohmann::json &out_json);

// Read up to max_bytes from the beginning of a file.
VxCoreError ReadFileHead(const std::filesystem::path &path, size_t max_bytes,
                         std::string &out_content);

// Assuming that all paths here are already cleaned using |CleanPath|.

std::string ConcatenatePaths(const std::string &parent_path, const std::string &child_name);

// Reads filesystem timestamps for the node at utf8_path (file or directory).
// out_modified_ms = last-write time (milliseconds since the Unix epoch).
// out_created_ms  = OS creation/birth time where the platform/filesystem
//                   supports it, otherwise equal to the modified time.
// Either out pointer may be null. Returns false on stat failure (outputs left
// untouched), true on success.
bool GetFilesystemTimes(const std::string &utf8_path, int64_t *out_created_ms,
                        int64_t *out_modified_ms, FileAccess access = FileAccess::kDefault);

std::pair<std::string, std::string> SplitPath(const std::string &path);

std::vector<std::string> SplitPathComponents(const std::string &path);

std::string RelativePath(const std::string &base, const std::string &path);

}  // namespace vxcore

#endif
