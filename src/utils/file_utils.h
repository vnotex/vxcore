#ifndef VXCORE_FILE_UTILS_H
#define VXCORE_FILE_UTILS_H

#include <filesystem>
#include <string>
#include <utility>

namespace vxcore {

inline std::string CleanFsPath(const std::filesystem::path &path) {
  if (path.empty()) {
    return ".";
  }
  return path.lexically_normal().generic_string();
}

inline std::string CleanPath(const std::string &path) {
  if (path.empty()) {
    return ".";
  }
  return CleanFsPath(std::filesystem::path(path));
}

// Assuming that all paths here are already cleaned using |CleanPath|.

std::string ConcatenatePaths(const std::string &parent_path, const std::string &child_name);

std::pair<std::string, std::string> SplitPath(const std::string &path);

std::string RelativePath(const std::string &base, const std::string &path);

}  // namespace vxcore

#endif
