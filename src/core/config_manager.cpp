#include "config_manager.h"

#include <fstream>

#include "platform/path_provider.h"
#include "utils/file_utils.h"
#include "utils/logger.h"

namespace vxcore {

const char *kCoreConfigFileName = "vxcore.json";
const char *kSessionConfigFileName = "session.json";
const char *kPortableConfigFolderName = "config";
const char *kTestConfigFolderName = "vxcore_test_config";

bool ConfigManager::test_mode_ = false;

ConfigManager::ConfigManager() : config_(), session_config_() {
  if (test_mode_) {
    std::filesystem::path temp_path(GetDataPathInTestMode());

    std::error_code ec;
    std::filesystem::remove_all(temp_path, ec);
    if (ec) {
      VXCORE_LOG_WARN("Failed to clear test data path: %s", ec.message().c_str());
    }

    app_data_path_ = temp_path;
    local_data_path_ = temp_path;
  } else {
    auto portable_config_path = PathProvider::GetExecutablePath() / kPortableConfigFolderName;
    if (std::filesystem::exists(portable_config_path)) {
      app_data_path_ = portable_config_path;
      local_data_path_ = portable_config_path;
    } else {
      app_data_path_ = PathProvider::GetAppDataPath();
      local_data_path_ = PathProvider::GetLocalDataPath();
    }
  }
}

std::string ConfigManager::GetDataPathInTestMode() {
  return (std::filesystem::temp_directory_path() / kTestConfigFolderName).string();
}

VxCoreError ConfigManager::LoadConfigs() {
  VXCORE_LOG_INFO("Loading configs");

  VxCoreError err = EnsureDataFolders();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to ensure data folders: error=%d", err);
    return err;
  }

  // We do not have a built-in default config file
  auto default_json = config_.ToJson();
  nlohmann::json user_json = nlohmann::json::object();
  auto user_config_path = app_data_path_ / kCoreConfigFileName;
  if (std::filesystem::exists(user_config_path)) {
    VXCORE_LOG_DEBUG("Loading user config: %s", user_config_path.string().c_str());
    err = LoadJsonFile(user_config_path, user_json);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Failed to load user config: error=%d", err);
      return err;
    }
  }

  nlohmann::json session_json = nlohmann::json::object();
  auto session_config_path = local_data_path_ / kSessionConfigFileName;
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
