#ifndef VXCORE_PATH_PROVIDER_H
#define VXCORE_PATH_PROVIDER_H

#include <filesystem>
#include <string>

namespace vxcore {

class PathProvider {
public:
  static std::filesystem::path getAppDataPath();
  static std::filesystem::path getLocalDataPath();
  static std::filesystem::path getExecutablePath();

private:
  static constexpr const char *kAppName = "VNote";
};

} // namespace vxcore

#endif
