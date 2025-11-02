#include "config_manager.h"

#include "platform/path_provider.h"

#include <fstream>

namespace vxcore {

ConfigManager::ConfigManager() : config_(), session_config_() {
  auto portable_config_path = PathProvider::getExecutablePath() / "config";
  if (std::filesystem::exists(portable_config_path)) {
    app_data_path_ = portable_config_path;
    local_data_path_ = portable_config_path;
  } else {
    app_data_path_ = PathProvider::getAppDataPath();
    local_data_path_ = PathProvider::getLocalDataPath();
  }
}

VxCoreError ConfigManager::loadConfigs() {
  VxCoreError err = ensureDataFolders();
  if (err != VXCORE_OK) {
    return err;
  }

  err = checkAndMigrateVersion();
  if (err != VXCORE_OK) {
    return err;
  }

  nlohmann::json default_json = nlohmann::json::object();
  auto default_config_path = PathProvider::getExecutablePath() / "data" / "vxcore.json";
  if (std::filesystem::exists(default_config_path)) {
    err = loadJsonFile(default_config_path, default_json);
    if (err != VXCORE_OK) {
      return err;
    }
  }

  nlohmann::json user_json = nlohmann::json::object();
  auto user_config_path = app_data_path_ / "vxcore.json";
  if (std::filesystem::exists(user_config_path)) {
    err = loadJsonFile(user_config_path, user_json);
    if (err != VXCORE_OK) {
      return err;
    }
  }

  nlohmann::json session_json = nlohmann::json::object();
  auto session_config_path = local_data_path_ / "session.json";
  if (std::filesystem::exists(session_config_path)) {
    err = loadJsonFile(session_config_path, session_json);
    if (err != VXCORE_OK) {
      return err;
    }
  }

  default_json.merge_patch(user_json);
  config_ = VxCoreConfig::fromJson(default_json);
  session_config_ = VxCoreSessionConfig::fromJson(session_json);

  return VXCORE_OK;
}

VxCoreError ConfigManager::loadJsonFile(const std::filesystem::path &path,
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

VxCoreError ConfigManager::ensureDataFolders() {
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

VxCoreError ConfigManager::checkAndMigrateVersion() { return VXCORE_OK; }

VxCoreError ConfigManager::saveSessionConfig() {
  if (local_data_path_.empty()) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    auto session_config_path = local_data_path_ / "session.json";
    std::ofstream file(session_config_path);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }
    nlohmann::json json = session_config_.toJson();
    file << json.dump(2);
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

} // namespace vxcore
