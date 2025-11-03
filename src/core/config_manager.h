#ifndef VXCORE_CONFIG_MANAGER_H
#define VXCORE_CONFIG_MANAGER_H

#include "vxcore/vxcore_types.h"
#include "vxcore_config.h"
#include "vxcore_session_config.h"

#include <filesystem>
#include <nlohmann/json.hpp>

namespace vxcore {

class ConfigManager {
public:
  ConfigManager();

  VxCoreError loadConfigs();

  const VxCoreConfig &getConfig() const { return config_; }
  const VxCoreSessionConfig &getSessionConfig() const { return session_config_; }
  VxCoreSessionConfig &getSessionConfig() { return session_config_; }

  std::string getLocalDataPath() const { return local_data_path_.string(); }

  VxCoreError saveSessionConfig();

private:
  VxCoreError loadJsonFile(const std::filesystem::path &path, nlohmann::json &out_json);
  VxCoreError ensureDataFolders();
  VxCoreError checkAndMigrateVersion();

  VxCoreConfig config_;
  VxCoreSessionConfig session_config_;
  std::filesystem::path app_data_path_;
  std::filesystem::path local_data_path_;
};

} // namespace vxcore

#endif
