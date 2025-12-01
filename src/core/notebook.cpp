#include "notebook.h"

#include <vxcore/vxcore_types.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

NotebookConfig::NotebookConfig()
    : assets_folder("vx_assets"),
      attachments_folder("vx_attachments"),
      metadata(nlohmann::json::object()) {}

NotebookConfig NotebookConfig::FromJson(const nlohmann::json &json) {
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
    config.assets_folder = json["assetsFolder"].get<std::string>();
  }
  if (json.contains("attachmentsFolder") && json["attachmentsFolder"].is_string()) {
    config.attachments_folder = json["attachmentsFolder"].get<std::string>();
  }
  if (json.contains("metadata") && json["metadata"].is_object()) {
    config.metadata = json["metadata"];
  }
  return config;
}

nlohmann::json NotebookConfig::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  json["id"] = id;
  json["name"] = name;
  json["description"] = description;
  json["assetsFolder"] = assets_folder;
  json["attachmentsFolder"] = attachments_folder;
  json["metadata"] = metadata;
  return json;
}

NotebookRecord::NotebookRecord() : type(NotebookType::Bundled), last_opened_timestamp(0) {}

NotebookRecord NotebookRecord::FromJson(const nlohmann::json &json) {
  NotebookRecord record;
  if (json.contains("id") && json["id"].is_string()) {
    record.id = json["id"].get<std::string>();
  }
  if (json.contains("rootFolder") && json["rootFolder"].is_string()) {
    record.root_folder = json["rootFolder"].get<std::string>();
  }
  if (json.contains("type") && json["type"].is_string()) {
    std::string typeStr = json["type"].get<std::string>();
    record.type = (typeStr == "raw") ? NotebookType::Raw : NotebookType::Bundled;
  }
  if (json.contains("lastOpenedTimestamp") && json["lastOpenedTimestamp"].is_number()) {
    record.last_opened_timestamp = json["lastOpenedTimestamp"].get<int64_t>();
  }
  if (json.contains("rawConfig") && json["rawConfig"].is_object()) {
    record.raw_config = NotebookConfig::FromJson(json["rawConfig"]);
  }
  return record;
}

nlohmann::json NotebookRecord::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  json["id"] = id;
  json["rootFolder"] = root_folder;
  json["type"] = (type == NotebookType::Raw) ? "raw" : "bundled";
  json["lastOpenedTimestamp"] = last_opened_timestamp;
  if (type == NotebookType::Raw) {
    json["rawConfig"] = raw_config.ToJson();
  }
  return json;
}

Notebook::Notebook(const std::string &root_folder, NotebookType type)
    : root_folder_(root_folder), type_(type) {}

VxCoreError Notebook::CreateBundledNotebook(const std::string &root_folder,
                                            const NotebookConfig *overridden_config,
                                            std::unique_ptr<Notebook> &out_notebook) {
  out_notebook.reset(new Notebook(root_folder, NotebookType::Bundled));
  if (overridden_config) {
    out_notebook->config_ = *overridden_config;
  }
  out_notebook->EnsureId();
  auto error = out_notebook->UpdateConfig(out_notebook->config_);
  if (error != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to save bundled notebook config: root=%s, error=%d",
                     root_folder.c_str(), error);
    out_notebook.reset();
    return error;
  }
  return VXCORE_OK;
}

VxCoreError Notebook::CreateRawNotebook(const std::string &root_folder,
                                        const NotebookConfig &config,
                                        std::unique_ptr<Notebook> &out_notebook) {
  out_notebook.reset();
  std::filesystem::path rootPath(root_folder);
  if (!std::filesystem::exists(rootPath)) {
    VXCORE_LOG_ERROR("Failed to create raw notebook: root=%s, error=%d", root_folder.c_str(),
                     VXCORE_ERR_NOT_FOUND);
    return VXCORE_ERR_NOT_FOUND;
  }

  out_notebook.reset(new Notebook(root_folder, NotebookType::Raw));
  out_notebook->config_ = config;
  out_notebook->EnsureId();
  return VXCORE_OK;
}

VxCoreError Notebook::FromBundledNotebook(const std::string &root_folder,
                                          std::unique_ptr<Notebook> &out_notebook) {
  out_notebook.reset(new Notebook(root_folder, NotebookType::Bundled));
  auto err = out_notebook->LoadConfig();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to load bundled notebook config: root=%s, error=%d",
                     root_folder.c_str(), err);
    out_notebook.reset();
    return err;
  }
  return VXCORE_OK;
}

void Notebook::EnsureId() {
  if (config_.id.empty()) {
    config_.id = GenerateUUID();
  }
}

std::string Notebook::GetDbPath(const std::string &local_data_folder) const {
  std::filesystem::path dbPath(local_data_folder);
  dbPath /= "notebooks";
  dbPath /= (config_.id + ".db");
  return dbPath.string();
}

std::string Notebook::GetMetadataFolder() const {
  std::filesystem::path metaPath(root_folder_);
  metaPath /= "vx_notebook";
  return metaPath.string();
}

std::string Notebook::GetConfigPath() const {
  std::filesystem::path configPath(GetMetadataFolder());
  configPath /= "config.json";
  return configPath.string();
}

VxCoreError Notebook::LoadConfig() {
  if (type_ == NotebookType::Raw) {
    return VXCORE_OK;
  }

  std::string configPath = GetConfigPath();
  std::ifstream file(configPath);
  if (!file.is_open()) {
    return VXCORE_ERR_IO;
  }

  try {
    nlohmann::json json;
    file >> json;
    auto config = NotebookConfig::FromJson(json);
    if (config.id.empty()) {
      return VXCORE_ERR_INVALID_STATE;
    }
    config_ = config;
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError Notebook::UpdateConfig(const NotebookConfig &config) {
  assert(config_.id == config_.id);

  config_ = config;

  if (type_ == NotebookType::Raw) {
    return VXCORE_OK;
  }

  try {
    std::filesystem::path metaPath(GetMetadataFolder());
    std::filesystem::create_directories(metaPath);

    std::string configPath = GetConfigPath();
    std::ofstream file(configPath);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }

    nlohmann::json json = config_.ToJson();
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

}  // namespace vxcore
