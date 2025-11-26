#include "config_manager.h"

#include <fstream>

#include "platform/path_provider.h"
#include "utils/logger.h"

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
  VXCORE_LOG_INFO("Loading configs");

  VxCoreError err = EnsureDataFolders();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to ensure data folders: error=%d", err);
    return err;
  }

  err = CheckAndMigrateVersion();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to check/migrate version: error=%d", err);
    return err;
  }

  nlohmann::json default_json = nlohmann::json::object();
  auto default_config_path = PathProvider::GetExecutablePath() / "data" / "vxcore.json";
  if (std::filesystem::exists(default_config_path)) {
    VXCORE_LOG_DEBUG("Loading default config: %s", default_config_path.string().c_str());
    err = LoadJsonFile(default_config_path, default_json);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Failed to load default config: error=%d", err);
      return err;
    }
  }

  nlohmann::json user_json = nlohmann::json::object();
  auto user_config_path = app_data_path_ / "vxcore.json";
  if (std::filesystem::exists(user_config_path)) {
    VXCORE_LOG_DEBUG("Loading user config: %s", user_config_path.string().c_str());
    err = LoadJsonFile(user_config_path, user_json);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Failed to load user config: error=%d", err);
      return err;
    }
  }

  nlohmann::json session_json = nlohmann::json::object();
  auto session_config_path = local_data_path_ / "session.json";
  if (std::filesystem::exists(session_config_path)) {
    VXCORE_LOG_DEBUG("Loading session config: %s", session_config_path.string().c_str());
    err = LoadJsonFile(session_config_path, session_json);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Failed to load session config: error=%d", err);
      return err;
    }
  }

  default_json.merge_patch(user_json);
  config_ = VxCoreConfig::FromJson(default_json);
  session_config_ = VxCoreSessionConfig::FromJson(session_json);

  VXCORE_LOG_INFO("Configs loaded successfully");
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
    VXCORE_LOG_ERROR("Cannot save session config: local_data_path not initialized");
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    auto session_config_path = local_data_path_ / "session.json";
    VXCORE_LOG_DEBUG("Saving session config: %s", session_config_path.string().c_str());
    std::ofstream file(session_config_path);
    if (!file.is_open()) {
      VXCORE_LOG_ERROR("Failed to open session config file for writing");
      return VXCORE_ERR_IO;
    }
    nlohmann::json json = session_config_.ToJson();
    file << json.dump(2);
    VXCORE_LOG_DEBUG("Session config saved successfully");
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    VXCORE_LOG_ERROR("JSON serialization error while saving session config: %s", e.what());
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    VXCORE_LOG_ERROR("Unknown error while saving session config");
    return VXCORE_ERR_IO;
  }
}

void ConfigManager::SetTestMode(bool enabled) { test_mode_ = enabled; }

bool ConfigManager::IsTestMode() { return test_mode_; }

}  // namespace vxcore
