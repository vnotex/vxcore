#include "notebook_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "utils/utils.h"

namespace vxcore {

NotebookManager::NotebookManager(const std::string &local_data_folder,
                                 VxCoreSessionConfig *session_config)
    : local_data_folder_(local_data_folder), session_config_(session_config) {
  LoadOpenNotebooks();
}

NotebookManager::~NotebookManager() {}

void NotebookManager::LoadOpenNotebooks() {
  if (!session_config_) {
    return;
  }

  notebooks_.clear();

  for (const auto &record : session_config_->notebooks) {
    try {
      std::filesystem::path rootPath(record.root_folder);
      if (!std::filesystem::exists(rootPath)) {
        continue;
      }

      NotebookType type = record.type;
      NotebookConfig config;

      if (type == NotebookType::Bundled) {
        std::filesystem::path configPath(rootPath);
        configPath /= "vx_notebook";
        configPath /= "config.json";

        if (!std::filesystem::exists(configPath)) {
          continue;
        }

        std::ifstream file(configPath);
        if (!file.is_open()) {
          continue;
        }

        nlohmann::json json;
        file >> json;
        config = NotebookConfig::FromJson(json);
      } else {
        config = record.raw_config;
      }

      auto notebook = std::make_unique<Notebook>(record.root_folder, type, config);
      notebooks_[notebook->GetId()] = std::move(notebook);
    } catch (...) {
    }
  }
}

VxCoreError NotebookManager::CreateNotebook(const std::string &root_folder, NotebookType type,
                                            const std::string &properties_json,
                                            std::string &out_notebook_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  try {
    std::filesystem::path rootPath(root_folder);
    if (!std::filesystem::exists(rootPath)) {
      std::filesystem::create_directories(rootPath);
    }

    NotebookConfig config;
    if (!properties_json.empty()) {
      nlohmann::json json = nlohmann::json::parse(properties_json);
      config = NotebookConfig::FromJson(json);
    }

    auto notebook = std::make_unique<Notebook>(root_folder, type, config);

    VxCoreError err = notebook->SaveConfig();
    if (err != VXCORE_OK) {
      return err;
    }

    out_notebook_id = notebook->GetId();

    NotebookRecord record;
    record.id = notebook->GetId();
    record.root_folder = root_folder;
    record.type = type;
    record.last_opened_timestamp = GetCurrentTimestampMillis();
    if (type == NotebookType::Raw) {
      record.raw_config = config;
    }

    err = SaveNotebookRecord(record);
    if (err != VXCORE_OK) {
      return err;
    }

    notebooks_[out_notebook_id] = std::move(notebook);

    UpdateSessionConfig();

    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::filesystem::filesystem_error &) {
    return VXCORE_ERR_IO;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::OpenNotebook(const std::string &root_folder,
                                          std::string &out_notebook_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  try {
    std::filesystem::path rootPath(root_folder);
    if (!std::filesystem::exists(rootPath)) {
      return VXCORE_ERR_NOT_FOUND;
    }

    NotebookRecord record;
    VxCoreError err = LoadNotebookRecord(root_folder, record);
    if (err != VXCORE_OK) {
      return err;
    }

    for (const auto &pair : notebooks_) {
      if (pair.second->GetRootFolder() == root_folder) {
        out_notebook_id = pair.first;
        return VXCORE_OK;
      }
    }

    NotebookType type = record.type;
    NotebookConfig config;

    if (type == NotebookType::Bundled) {
      std::filesystem::path configPath(rootPath);
      configPath /= "vx_notebook";
      configPath /= "config.json";

      if (!std::filesystem::exists(configPath)) {
        return VXCORE_ERR_NOT_FOUND;
      }

      std::ifstream file(configPath);
      if (!file.is_open()) {
        return VXCORE_ERR_IO;
      }

      nlohmann::json json;
      file >> json;
      config = NotebookConfig::FromJson(json);
    } else {
      config = record.raw_config;
    }

    auto notebook = std::make_unique<Notebook>(root_folder, type, config);
    out_notebook_id = notebook->GetId();

    record.last_opened_timestamp = GetCurrentTimestampMillis();
    err = SaveNotebookRecord(record);
    if (err != VXCORE_OK) {
      return err;
    }

    notebooks_[out_notebook_id] = std::move(notebook);

    UpdateSessionConfig();

    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::filesystem::filesystem_error &) {
    return VXCORE_ERR_IO;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::CloseNotebook(const std::string &notebook_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = notebooks_.find(notebook_id);
  if (it == notebooks_.end()) {
    return VXCORE_ERR_NOT_FOUND;
  }

  notebooks_.erase(it);

  UpdateSessionConfig();

  return VXCORE_OK;
}

VxCoreError NotebookManager::GetNotebookProperties(const std::string &notebook_id,
                                                   std::string &out_properties_json) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = notebooks_.find(notebook_id);
  if (it == notebooks_.end()) {
    return VXCORE_ERR_NOT_FOUND;
  }

  try {
    nlohmann::json json = it->second->GetConfig().ToJson();
    json["rootFolder"] = it->second->GetRootFolder();
    json["type"] = (it->second->GetType() == NotebookType::Raw) ? "raw" : "bundled";
    out_properties_json = json.dump();
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::SetNotebookProperties(const std::string &notebook_id,
                                                   const std::string &properties_json) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = notebooks_.find(notebook_id);
  if (it == notebooks_.end()) {
    return VXCORE_ERR_NOT_FOUND;
  }

  try {
    nlohmann::json json = nlohmann::json::parse(properties_json);
    NotebookConfig config = NotebookConfig::FromJson(json);
    config.id = notebook_id;

    it->second->SetConfig(config);

    VxCoreError err = it->second->SaveConfig();
    if (err != VXCORE_OK) {
      return err;
    }

    NotebookRecord record;
    record.id = notebook_id;
    record.root_folder = it->second->GetRootFolder();
    record.type = it->second->GetType();
    record.last_opened_timestamp = GetCurrentTimestampMillis();
    if (record.type == NotebookType::Raw) {
      record.raw_config = config;
    }

    err = SaveNotebookRecord(record);
    if (err != VXCORE_OK) {
      return err;
    }

    UpdateSessionConfig();

    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::ListNotebooks(std::string &out_notebooks_json) {
  std::lock_guard<std::mutex> lock(mutex_);

  try {
    nlohmann::json jsonArray = nlohmann::json::array();

    for (const auto &pair : notebooks_) {
      nlohmann::json item = nlohmann::json::object();
      item["id"] = pair.second->GetId();
      item["rootFolder"] = pair.second->GetRootFolder();
      item["type"] = (pair.second->GetType() == NotebookType::Raw) ? "raw" : "bundled";
      item["config"] = pair.second->GetConfig().ToJson();
      jsonArray.push_back(item);
    }

    out_notebooks_json = jsonArray.dump();
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

Notebook *NotebookManager::GetNotebook(const std::string &notebook_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = notebooks_.find(notebook_id);
  if (it == notebooks_.end()) {
    return nullptr;
  }
  return it->second.get();
}

void NotebookManager::SetSessionConfigUpdater(std::function<void()> updater) {
  session_config_updater_ = std::move(updater);
}

VxCoreError NotebookManager::LoadNotebookRecord(const std::string &root_folder,
                                                NotebookRecord &record) {
  if (session_config_) {
    for (const auto &existingRecord : session_config_->notebooks) {
      if (existingRecord.root_folder == root_folder) {
        record = existingRecord;
        return VXCORE_OK;
      }
    }
  }

  std::filesystem::path rootPath(root_folder);
  std::filesystem::path metaPath = rootPath / "vx_notebook";
  std::filesystem::path configPath = metaPath / "config.json";

  bool isBundled = std::filesystem::exists(configPath);

  if (isBundled) {
    std::ifstream file(configPath);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }

    try {
      nlohmann::json json;
      file >> json;
      NotebookConfig config = NotebookConfig::FromJson(json);

      record.id = config.id;
      record.root_folder = root_folder;
      record.type = NotebookType::Bundled;
      record.last_opened_timestamp = 0;
      return VXCORE_OK;
    } catch (const nlohmann::json::exception &) {
      return VXCORE_ERR_JSON_PARSE;
    }
  } else {
    return VXCORE_ERR_NOT_FOUND;
  }
}

VxCoreError NotebookManager::SaveNotebookRecord(const NotebookRecord &record) {
  if (!session_config_) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  NotebookRecord *existing = FindNotebookRecord(record.id);
  if (existing) {
    *existing = record;
  } else {
    session_config_->notebooks.push_back(record);
  }

  return VXCORE_OK;
}

NotebookRecord *NotebookManager::FindNotebookRecord(const std::string &id) {
  if (!session_config_) {
    return nullptr;
  }

  auto it = std::find_if(session_config_->notebooks.begin(), session_config_->notebooks.end(),
                         [&id](const NotebookRecord &r) { return r.id == id; });

  if (it != session_config_->notebooks.end()) {
    return &(*it);
  }

  return nullptr;
}

void NotebookManager::UpdateSessionConfig() {
  if (session_config_updater_) {
    session_config_updater_();
  }
}

}  // namespace vxcore
