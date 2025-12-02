#ifndef VXCORE_FILE_UTILS_H
#define VXCORE_FILE_UTILS_H

#include <string>
#include <utility>

namespace vxcore {

std::string ConcatenatePaths(const std::string &parent_path, const std::string &child_name);

std::pair<std::string, std::string> SplitPath(const std::string &path);

}  // namespace vxcore

#endif
