#ifndef VXCORE_FILE_UTILS_H
#define VXCORE_FILE_UTILS_H

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "vxcore/vxcore_types.h"

namespace vxcore {

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
ReparseState CheckReparsePoint(const std::string &path);

// True if |path| is a reparse point.
// Windows: any FILE_ATTRIBUTE_REPARSE_POINT, which covers symlinks, directory
//          junctions (IO_REPARSE_TAG_MOUNT_POINT) and other redirecting or
//          virtualizing tags. std::filesystem::is_symlink() is NOT sufficient
//          here: MSVC's STL maps junctions to the implementation-defined
//          file_type::junction, which is_symlink() does not report.
// POSIX:   symlink_status().type() == file_type::symlink.
// Never follows the link. Returns |on_error| if the path cannot be stat'ed.
bool IsReparsePoint(const std::string &path, bool on_error);

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
                        int64_t *out_modified_ms);

std::pair<std::string, std::string> SplitPath(const std::string &path);

std::vector<std::string> SplitPathComponents(const std::string &path);

std::string RelativePath(const std::string &base, const std::string &path);

}  // namespace vxcore

#endif
