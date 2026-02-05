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
std::string ConfigManager::org_name_ = "VNoteX";
std::string ConfigManager::app_name_ = "vxcore";

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
      app_data_path_ = PathProvider::GetAppDataPath(app_name_);
      local_data_path_ = PathProvider::GetLocalDataPath(app_name_);
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

void ConfigManager::SetAppInfo(const std::string &org_name, const std::string &app_name) {
  org_name_ = org_name;
  app_name_ = app_name;
}

bool ConfigManager::IsValidConfigBaseName(const std::string &base_name) const {
  if (base_name.empty()) {
    return false;
  }
  // Reject path separators and parent directory references
  if (base_name.find('/') != std::string::npos || base_name.find('\\') != std::string::npos ||
      base_name.find("..") != std::string::npos) {
    return false;
  }
  return true;
}

VxCoreError ConfigManager::LoadConfigByName(VxCoreDataLocation location,
                                            const std::string &base_name,
                                            std::string &out_content) {
  if (!IsValidConfigBaseName(base_name)) {
    VXCORE_LOG_ERROR("Invalid config base name: %s", base_name.c_str());
    return VXCORE_ERR_INVALID_PARAM;
  }

  std::filesystem::path base_path =
      (location == VXCORE_DATA_APP) ? app_data_path_ : local_data_path_;
  std::filesystem::path config_path = base_path / (base_name + ".json");

  if (!std::filesystem::exists(config_path)) {
    VXCORE_LOG_DEBUG("Config file not found: %s", config_path.string().c_str());
    return VXCORE_ERR_NOT_FOUND;
  }

  VXCORE_LOG_DEBUG("Loading config by name: %s", config_path.string().c_str());
  VxCoreError err = ReadFile(config_path, out_content);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to read config file: %s", config_path.string().c_str());
  }
  return err;
}

VxCoreError ConfigManager::SaveConfigByName(VxCoreDataLocation location,
                                            const std::string &base_name,
                                            const std::string &content) {
  if (!IsValidConfigBaseName(base_name)) {
    VXCORE_LOG_ERROR("Invalid config base name: %s", base_name.c_str());
    return VXCORE_ERR_INVALID_PARAM;
  }

  std::filesystem::path base_path =
      (location == VXCORE_DATA_APP) ? app_data_path_ : local_data_path_;

  try {
    std::filesystem::create_directories(base_path);
  } catch (...) {
    VXCORE_LOG_ERROR("Failed to create directory: %s", base_path.string().c_str());
    return VXCORE_ERR_IO;
  }

  std::filesystem::path config_path = base_path / (base_name + ".json");
  VXCORE_LOG_DEBUG("Saving config by name: %s", config_path.string().c_str());

  VxCoreError err = WriteFile(config_path, content);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to write config file: %s", config_path.string().c_str());
  }
  return err;
}

VxCoreError ConfigManager::LoadJsonWithDefaults(const std::string &file_path,
                                                const std::string &defaults_json,
                                                std::string &out_merged) {
  if (file_path.empty()) {
    VXCORE_LOG_ERROR("LoadJsonWithDefaults: file_path is empty");
    return VXCORE_ERR_INVALID_PARAM;
  }

  try {
    // Parse defaults
    nlohmann::json default_json = nlohmann::json::parse(defaults_json);

    // Load user file if exists
    nlohmann::json user_json = nlohmann::json::object();
    std::filesystem::path path(file_path);
    if (std::filesystem::exists(path)) {
      VXCORE_LOG_DEBUG("Loading JSON with defaults: %s", file_path.c_str());
      VxCoreError err = LoadJsonFile(path, user_json);
      if (err != VXCORE_OK) {
        VXCORE_LOG_ERROR("Failed to load JSON file: %s", file_path.c_str());
        return err;
      }
    } else {
      VXCORE_LOG_DEBUG("File not found, using defaults: %s", file_path.c_str());
    }

    // Merge user config on top of defaults using merge_patch
    default_json.merge_patch(user_json);
    out_merged = default_json.dump(2);
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    VXCORE_LOG_ERROR("JSON error in LoadJsonWithDefaults: %s", e.what());
    return VXCORE_ERR_JSON_PARSE;
  } catch (...) {
    VXCORE_LOG_ERROR("Unknown error in LoadJsonWithDefaults");
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError ConfigManager::SaveJson(const std::string &file_path, const std::string &content) {
  if (file_path.empty()) {
    VXCORE_LOG_ERROR("SaveJson: file_path is empty");
    return VXCORE_ERR_INVALID_PARAM;
  }

  try {
    // Validate JSON before saving
    (void)nlohmann::json::parse(content);

    std::filesystem::path path(file_path);
    // Ensure parent directory exists
    if (path.has_parent_path()) {
      std::filesystem::create_directories(path.parent_path());
    }

    VXCORE_LOG_DEBUG("Saving JSON: %s", file_path.c_str());
    VxCoreError err = WriteFile(path, content);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Failed to write JSON file: %s", file_path.c_str());
      return err;
    }
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    VXCORE_LOG_ERROR("JSON error in SaveJson: %s", e.what());
    return VXCORE_ERR_JSON_PARSE;
  } catch (...) {
    VXCORE_LOG_ERROR("Unknown error in SaveJson");
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError ConfigManager::ReadJsonValue(const std::string &file_path, const std::string &json_path,
                                         std::string &out_value) {
  if (file_path.empty()) {
    VXCORE_LOG_ERROR("ReadJsonValue: file_path is empty");
    return VXCORE_ERR_INVALID_PARAM;
  }

  try {
    std::filesystem::path path(file_path);
    if (!std::filesystem::exists(path)) {
      VXCORE_LOG_DEBUG("ReadJsonValue: file not found: %s", file_path.c_str());
      return VXCORE_ERR_NOT_FOUND;
    }

    nlohmann::json json_obj;
    VxCoreError err = LoadJsonFile(path, json_obj);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("ReadJsonValue: failed to load file: %s", file_path.c_str());
      return err;
    }

    // Navigate JSON path (e.g., "core.theme" -> json["core"]["theme"])
    if (json_path.empty()) {
      // Return entire JSON
      out_value = json_obj.dump(2);
      return VXCORE_OK;
    }

    nlohmann::json current = json_obj;
    size_t start = 0;
    size_t dot_pos = json_path.find('.');
    while (dot_pos != std::string::npos) {
      std::string key = json_path.substr(start, dot_pos - start);
      if (!current.contains(key)) {
        VXCORE_LOG_DEBUG("ReadJsonValue: key not found: %s", key.c_str());
        return VXCORE_ERR_NOT_FOUND;
      }
      current = current[key];
      start = dot_pos + 1;
      dot_pos = json_path.find('.', start);
    }

    // Final key
    std::string final_key = json_path.substr(start);
    if (!current.contains(final_key)) {
      VXCORE_LOG_DEBUG("ReadJsonValue: key not found: %s", final_key.c_str());
      return VXCORE_ERR_NOT_FOUND;
    }

    current = current[final_key];
    if (current.is_string()) {
      out_value = current.get<std::string>();
    } else {
      out_value = current.dump();
    }
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    VXCORE_LOG_ERROR("JSON error in ReadJsonValue: %s", e.what());
    return VXCORE_ERR_JSON_PARSE;
  } catch (...) {
    VXCORE_LOG_ERROR("Unknown error in ReadJsonValue");
    return VXCORE_ERR_UNKNOWN;
  }
}

}  // namespace vxcore
