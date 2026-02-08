#ifndef VXCORE_PATH_PROVIDER_H
#define VXCORE_PATH_PROVIDER_H

#include <filesystem>
#include <string>

namespace vxcore {

class PathProvider {
 public:
  static std::filesystem::path GetAppDataPath(const std::string &app_name);
  static std::filesystem::path GetLocalDataPath(const std::string &app_name);
  static std::filesystem::path GetExecutionFolderPath();
  static std::filesystem::path GetExecutionFilePath();
};

}  // namespace vxcore

#endif
