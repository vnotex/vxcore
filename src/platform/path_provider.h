#ifndef VXCORE_PATH_PROVIDER_H
#define VXCORE_PATH_PROVIDER_H

#include <filesystem>
#include <string>

namespace vxcore {

class PathProvider {
 public:
  static std::filesystem::path GetAppDataPath();
  static std::filesystem::path GetLocalDataPath();
  static std::filesystem::path GetExecutablePath();

 private:
  static constexpr const char *kAppName = "VNote";
};

}  // namespace vxcore

#endif
