#include "file_utils.h"

#include <filesystem>

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
  size_t last_slash = path.find_last_of("/\\");
  if (last_slash == std::string::npos) {
    return {".", path};
  } else {
    std::string parent_path = path.substr(0, last_slash);
    std::string child_name = path.substr(last_slash + 1);
    return {parent_path, child_name};
  }
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

}  // namespace vxcore
