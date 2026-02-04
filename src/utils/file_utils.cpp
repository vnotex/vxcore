#include "file_utils.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace vxcore {

static constexpr char kPathSeparator = '/';

std::string ConcatenatePaths(const std::string &parent_path, const std::string &child_name) {
  if (parent_path.empty() || parent_path == ".") {
    return child_name;
  } else {
    return parent_path + kPathSeparator + child_name;
  }
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

bool IsRelativePath(const std::string &path) { return std::filesystem::path(path).is_relative(); }

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
