#include "notebook.h"

#include "utils/utils.h"

#include <vxcore/vxcore_types.h>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace vxcore {

NotebookConfig::NotebookConfig()
    : assetsFolder("vx_assets"), attachmentsFolder("vx_attachments"),
      metadata(nlohmann::json::object()) {}

NotebookConfig NotebookConfig::fromJson(const nlohmann::json &json) {
  NotebookConfig config;
  if (json.contains("id") && json["id"].is_string()) {
    config.id = json["id"].get<std::string>();
  }
  if (json.contains("name") && json["name"].is_string()) {
    config.name = json["name"].get<std::string>();
  }
  if (json.contains("description") && json["description"].is_string()) {
    config.description = json["description"].get<std::string>();
  }
  if (json.contains("assetsFolder") && json["assetsFolder"].is_string()) {
    config.assetsFolder = json["assetsFolder"].get<std::string>();
  }
  if (json.contains("attachmentsFolder") && json["attachmentsFolder"].is_string()) {
    config.attachmentsFolder = json["attachmentsFolder"].get<std::string>();
  }
  if (json.contains("metadata") && json["metadata"].is_object()) {
    config.metadata = json["metadata"];
  }
  return config;
}

nlohmann::json NotebookConfig::toJson() const {
  nlohmann::json json = nlohmann::json::object();
  json["id"] = id;
  json["name"] = name;
  json["description"] = description;
  json["assetsFolder"] = assetsFolder;
  json["attachmentsFolder"] = attachmentsFolder;
  json["metadata"] = metadata;
  return json;
}

NotebookRecord::NotebookRecord() : type(NotebookType::Bundled), lastOpenedTimestamp(0) {}

NotebookRecord NotebookRecord::fromJson(const nlohmann::json &json) {
  NotebookRecord record;
  if (json.contains("id") && json["id"].is_string()) {
    record.id = json["id"].get<std::string>();
  }
  if (json.contains("rootFolder") && json["rootFolder"].is_string()) {
    record.rootFolder = json["rootFolder"].get<std::string>();
  }
  if (json.contains("type") && json["type"].is_string()) {
    std::string typeStr = json["type"].get<std::string>();
    record.type = (typeStr == "raw") ? NotebookType::Raw : NotebookType::Bundled;
  }
  if (json.contains("lastOpenedTimestamp") && json["lastOpenedTimestamp"].is_number()) {
    record.lastOpenedTimestamp = json["lastOpenedTimestamp"].get<int64_t>();
  }
  if (json.contains("rawConfig") && json["rawConfig"].is_object()) {
    record.rawConfig = NotebookConfig::fromJson(json["rawConfig"]);
  }
  return record;
}

nlohmann::json NotebookRecord::toJson() const {
  nlohmann::json json = nlohmann::json::object();
  json["id"] = id;
  json["rootFolder"] = rootFolder;
  json["type"] = (type == NotebookType::Raw) ? "raw" : "bundled";
  json["lastOpenedTimestamp"] = lastOpenedTimestamp;
  if (type == NotebookType::Raw) {
    json["rawConfig"] = rawConfig.toJson();
  }
  return json;
}

Notebook::Notebook(const std::string &rootFolder, NotebookType type, const NotebookConfig &config)
    : rootFolder_(rootFolder), type_(type), config_(config) {
  if (config_.id.empty()) {
    config_.id = generateUUID();
  }
}

void Notebook::setConfig(const NotebookConfig &config) { config_ = config; }

std::string Notebook::getDbPath(const std::string &localDataFolder) const {
  std::filesystem::path dbPath(localDataFolder);
  dbPath /= "notebooks";
  dbPath /= (config_.id + ".db");
  return dbPath.string();
}

std::string Notebook::getMetadataFolder() const {
  std::filesystem::path metaPath(rootFolder_);
  metaPath /= "vx_notebook";
  return metaPath.string();
}

std::string Notebook::getConfigPath() const {
  std::filesystem::path configPath(getMetadataFolder());
  configPath /= "config.json";
  return configPath.string();
}

VxCoreError Notebook::loadConfig() {
  if (type_ == NotebookType::Raw) {
    return VXCORE_OK;
  }

  std::string configPath = getConfigPath();
  std::ifstream file(configPath);
  if (!file.is_open()) {
    return VXCORE_ERR_IO;
  }

  try {
    nlohmann::json json;
    file >> json;
    config_ = NotebookConfig::fromJson(json);
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError Notebook::saveConfig() {
  if (type_ == NotebookType::Raw) {
    return VXCORE_OK;
  }

  try {
    std::filesystem::path metaPath(getMetadataFolder());
    std::filesystem::create_directories(metaPath);

    std::string configPath = getConfigPath();
    std::ofstream file(configPath);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }

    nlohmann::json json = config_.toJson();
    file << json.dump(2);
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (const std::filesystem::filesystem_error &) {
    return VXCORE_ERR_IO;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

} // namespace vxcore
