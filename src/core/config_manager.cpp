#include "config_manager.h"

#include <fstream>

#include "platform/path_provider.h"

namespace vxcore {

bool ConfigManager::test_mode_ = false;

ConfigManager::ConfigManager() : config_(), session_config_() {
  if (test_mode_) {
    auto temp_path = std::filesystem::temp_directory_path() / "vxcore_test";
    app_data_path_ = temp_path;
    local_data_path_ = temp_path;
  } else {
    auto portable_config_path = PathProvider::GetExecutablePath() / "config";
    if (std::filesystem::exists(portable_config_path)) {
      app_data_path_ = portable_config_path;
      local_data_path_ = portable_config_path;
    } else {
      app_data_path_ = PathProvider::GetAppDataPath();
      local_data_path_ = PathProvider::GetLocalDataPath();
    }
  }
}

VxCoreError ConfigManager::LoadConfigs() {
  VxCoreError err = EnsureDataFolders();
  if (err != VXCORE_OK) {
    return err;
  }

  err = CheckAndMigrateVersion();
  if (err != VXCORE_OK) {
    return err;
  }

  nlohmann::json default_json = nlohmann::json::object();
  auto default_config_path = PathProvider::GetExecutablePath() / "data" / "vxcore.json";
  if (std::filesystem::exists(default_config_path)) {
    err = LoadJsonFile(default_config_path, default_json);
    if (err != VXCORE_OK) {
      return err;
    }
  }

  nlohmann::json user_json = nlohmann::json::object();
  auto user_config_path = app_data_path_ / "vxcore.json";
  if (std::filesystem::exists(user_config_path)) {
    err = LoadJsonFile(user_config_path, user_json);
    if (err != VXCORE_OK) {
      return err;
    }
  }

  nlohmann::json session_json = nlohmann::json::object();
  auto session_config_path = local_data_path_ / "session.json";
  if (std::filesystem::exists(session_config_path)) {
    err = LoadJsonFile(session_config_path, session_json);
    if (err != VXCORE_OK) {
      return err;
    }
  }

  default_json.merge_patch(user_json);
  config_ = VxCoreConfig::FromJson(default_json);
  session_config_ = VxCoreSessionConfig::FromJson(session_json);

  return VXCORE_OK;
}

VxCoreError ConfigManager::LoadJsonFile(const std::filesystem::path &path,
                                        nlohmann::json &out_json) {
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

VxCoreError ConfigManager::EnsureDataFolders() {
  try {
    if (!app_data_path_.empty()) {
      std::filesystem::create_directories(app_data_path_);
    }
    if (!local_data_path_.empty()) {
      std::filesystem::create_directories(local_data_path_);
    }
    return VXCORE_OK;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

VxCoreError ConfigManager::CheckAndMigrateVersion() { return VXCORE_OK; }

VxCoreError ConfigManager::SaveSessionConfig() {
  if (local_data_path_.empty()) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    auto session_config_path = local_data_path_ / "session.json";
    std::ofstream file(session_config_path);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }
    nlohmann::json json = session_config_.ToJson();
    file << json.dump(2);
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

void ConfigManager::SetTestMode(bool enabled) { test_mode_ = enabled; }

bool ConfigManager::IsTestMode() { return test_mode_; }

}  // namespace vxcore
