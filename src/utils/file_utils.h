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

std::pair<std::string, std::string> SplitPath(const std::string &path);

std::vector<std::string> SplitPathComponents(const std::string &path);

std::string RelativePath(const std::string &base, const std::string &path);

}  // namespace vxcore

#endif
