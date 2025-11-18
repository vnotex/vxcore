#ifndef VXCORE_CONFIG_MANAGER_H
#define VXCORE_CONFIG_MANAGER_H

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

#include "vxcore/vxcore_types.h"
#include "vxcore_config.h"
#include "vxcore_session_config.h"

namespace vxcore {

class ConfigManager {
 public:
  ConfigManager();

  VxCoreError LoadConfigs();

  const VxCoreConfig &GetConfig() const { return config_; }
  const VxCoreSessionConfig &GetSessionConfig() const { return session_config_; }
  VxCoreSessionConfig &GetSessionConfig() { return session_config_; }

  std::string GetLocalDataPath() const { return local_data_path_.string(); }
  std::string GetConfigPath() const { return (app_data_path_ / "vxcore.json").string(); }
  std::string GetSessionConfigPath() const { return (local_data_path_ / "session.json").string(); }

  VxCoreError SaveSessionConfig();

  static void SetTestMode(bool enabled);
  static bool IsTestMode();

 private:
  VxCoreError LoadJsonFile(const std::filesystem::path &path, nlohmann::json &out_json);
  VxCoreError EnsureDataFolders();
  VxCoreError CheckAndMigrateVersion();

  VxCoreConfig config_;
  VxCoreSessionConfig session_config_;
  std::filesystem::path app_data_path_;
  std::filesystem::path local_data_path_;

  static bool test_mode_;
};

}  // namespace vxcore

#endif
