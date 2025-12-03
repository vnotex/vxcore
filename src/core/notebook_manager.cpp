#include "notebook_manager.h"

#include <algorithm>
#include <filesystem>

#include "bundled_notebook.h"
#include "raw_notebook.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

NotebookManager::NotebookManager(VxCoreSessionConfig *session_config)
    : session_config_(session_config) {
  LoadOpenNotebooks();
}

NotebookManager::~NotebookManager() {}

void NotebookManager::LoadOpenNotebooks() {
  if (!session_config_) {
    return;
  }

  notebooks_.clear();

  for (const auto &record : session_config_->notebooks) {
    std::unique_ptr<Notebook> notebook;
    const std::string root_folder = CleanPath(record.root_folder);
    switch (record.type) {
      case NotebookType::Bundled: {
        auto error = BundledNotebook::Create(root_folder, nullptr, notebook);
        if (error != VXCORE_OK) {
          VXCORE_LOG_ERROR("Failed to load bundled notebook: root_folder=%s, error=%d",
                           record.root_folder.c_str(), error);
          continue;
        }
        break;
      }
      case NotebookType::Raw: {
        auto error = RawNotebook::Create(root_folder, record.raw_config, notebook);
        if (error != VXCORE_OK) {
          VXCORE_LOG_ERROR("Failed to load raw notebook: root_folder=%s, error=%d",
                           record.root_folder.c_str(), error);
          continue;
        }
        break;
      }
      default:
        VXCORE_LOG_ERROR("Skip invalid notebook type: %d", static_cast<int>(record.type));
        break;
    }

    if (notebook) {
      VXCORE_LOG_INFO("Loaded open notebook: id=%s, root_folder=%s", notebook->GetId().c_str(),
                      notebook->GetRootFolder().c_str());
      notebooks_[notebook->GetId()] = std::move(notebook);
    }
  }
}

VxCoreError NotebookManager::CreateNotebook(const std::string &root_folder, NotebookType type,
                                            const std::string &config_json,
                                            std::string &out_notebook_id) {
  VXCORE_LOG_INFO("Creating notebook: root_folder=%s, type=%d", root_folder.c_str(),
                  static_cast<int>(type));

  const auto root_folder_clean = CleanPath(root_folder);

  try {
    std::filesystem::path rootPath(root_folder_clean);
    if (!std::filesystem::exists(rootPath)) {
      std::filesystem::create_directories(rootPath);
      VXCORE_LOG_DEBUG("Created root directory: %s", root_folder_clean.c_str());
    } else {
      // TODO: check if @root_folder already has a notebook.
    }

    // At least there should be name.
    assert(!config_json.empty());
    nlohmann::json json = nlohmann::json::parse(config_json);
    auto config = NotebookConfig::FromJson(json);
    config.id.clear();

    std::unique_ptr<Notebook> notebook;
    switch (type) {
      case NotebookType::Bundled: {
        auto err = BundledNotebook::Create(root_folder_clean, &config, notebook);
        if (err != VXCORE_OK) {
          VXCORE_LOG_ERROR("Failed to create bundled notebook: root_folder=%s, error=%d",
                           root_folder_clean.c_str(), err);
          return err;
        }
        break;
      }
      case NotebookType::Raw: {
        auto err = RawNotebook::Create(root_folder_clean, config, notebook);
        if (err != VXCORE_OK) {
          VXCORE_LOG_ERROR("Failed to create raw notebook: root_folder=%s, error=%d",
                           root_folder_clean.c_str(), err);
          return err;
        }
        break;
      }
      default:
        VXCORE_LOG_ERROR("Invalid notebook type: %d", static_cast<int>(type));
        return VXCORE_ERR_INVALID_PARAM;
    }

    auto err = UpdateNotebookRecord(*notebook);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Failed to save notebook record after creation: id=%s, error=%d",
                       notebook->GetId().c_str(), err);
      return err;
    }

    out_notebook_id = notebook->GetId();
    notebooks_[out_notebook_id] = std::move(notebook);

    VXCORE_LOG_INFO("Notebook created successfully: id=%s", out_notebook_id.c_str());
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    VXCORE_LOG_ERROR("JSON parse error while creating notebook: %s", e.what());
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::filesystem::filesystem_error &e) {
    VXCORE_LOG_ERROR("Filesystem error while creating notebook: %s", e.what());
    return VXCORE_ERR_IO;
  } catch (...) {
    VXCORE_LOG_ERROR("Unknown error while creating notebook");
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::OpenNotebook(const std::string &root_folder,
                                          std::string &out_notebook_id) {
  VXCORE_LOG_INFO("Opening notebook: root_folder=%s", root_folder.c_str());
  const auto root_folder_clean = CleanPath(root_folder);
  if (auto *notebook = FindNotebookByRootFolder(root_folder_clean)) {
    out_notebook_id = notebook->GetId();
    VXCORE_LOG_DEBUG("Notebook already open: id=%s", out_notebook_id.c_str());
    return VXCORE_OK;
  }

  std::filesystem::path rootPath(root_folder_clean);
  if (!std::filesystem::exists(rootPath)) {
    VXCORE_LOG_WARN("Notebook root folder not found: %s", root_folder_clean.c_str());
    return VXCORE_ERR_NOT_FOUND;
  }

  std::unique_ptr<Notebook> notebook;
  auto err = BundledNotebook::Open(root_folder_clean, notebook);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to load bundled notebook: root_folder=%s, error=%d",
                     root_folder_clean.c_str(), err);
    return err;
  }

  err = UpdateNotebookRecord(*notebook);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to save notebook record after open: id=%s, error=%d",
                     notebook->GetId().c_str(), err);
    return err;
  }

  out_notebook_id = notebook->GetId();
  notebooks_[out_notebook_id] = std::move(notebook);

  VXCORE_LOG_INFO("Notebook open successfully: id=%s", out_notebook_id.c_str());
  return VXCORE_OK;
}

VxCoreError NotebookManager::CloseNotebook(const std::string &notebook_id) {
  VXCORE_LOG_INFO("Closing notebook: id=%s", notebook_id.c_str());

  auto it = notebooks_.find(notebook_id);
  if (it == notebooks_.end()) {
    VXCORE_LOG_WARN("Notebook not found for closing: id=%s", notebook_id.c_str());
    return VXCORE_ERR_NOT_FOUND;
  }

  notebooks_.erase(it);

  if (session_config_updater_) {
    session_config_updater_();
  }

  VXCORE_LOG_INFO("Notebook closed successfully: id=%s", notebook_id.c_str());
  return VXCORE_OK;
}

VxCoreError NotebookManager::GetNotebookConfig(const std::string &notebook_id,
                                               std::string &out_config_json) {
  auto *notebook = GetNotebook(notebook_id);
  if (!notebook) {
    return VXCORE_ERR_NOT_FOUND;
  }

  try {
    nlohmann::json json = ToNotebookConfig(*notebook);
    out_config_json = json.dump();
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

nlohmann::json NotebookManager::ToNotebookConfig(const Notebook &notebook) const {
  nlohmann::json json = notebook.GetConfig().ToJson();
  json["rootFolder"] = notebook.GetRootFolder();
  json["type"] = notebook.GetTypeStr();
  return json;
}

VxCoreError NotebookManager::UpdateNotebookConfig(const std::string &notebook_id,
                                                  const std::string &config_json) {
  auto *notebook = GetNotebook(notebook_id);
  if (!notebook) {
    return VXCORE_ERR_NOT_FOUND;
  }

  try {
    nlohmann::json json = nlohmann::json::parse(config_json);
    NotebookConfig config = NotebookConfig::FromJson(json);
    config.id = notebook_id;

    VxCoreError err = notebook->UpdateConfig(config);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Failed to update notebook config: id=%s, error=%d", notebook_id.c_str(),
                       err);
      return err;
    }

    err = UpdateNotebookRecord(*notebook);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Failed to update notebook record after config update: id=%s, error=%d",
                       notebook_id.c_str(), err);
      return err;
    }

    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::ListNotebooks(std::string &out_notebooks_json) {
  try {
    nlohmann::json jsonArray = nlohmann::json::array();

    for (const auto &pair : notebooks_) {
      nlohmann::json item = ToNotebookConfig(*pair.second);
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
  auto it = notebooks_.find(notebook_id);
  if (it == notebooks_.end()) {
    return nullptr;
  }
  return it->second.get();
}

void NotebookManager::SetSessionConfigUpdater(std::function<void()> updater) {
  session_config_updater_ = std::move(updater);
}

VxCoreError NotebookManager::UpdateNotebookRecord(const Notebook &notebook) {
  NotebookRecord *record = FindNotebookRecord(notebook.GetId());
  if (!record) {
    session_config_->notebooks.emplace_back();
    record = &session_config_->notebooks.back();
  }
  record->id = notebook.GetId();
  record->root_folder = notebook.GetRootFolder();
  record->type = notebook.GetType();
  record->last_opened_timestamp = GetCurrentTimestampMillis();
  if (record->type == NotebookType::Raw) {
    record->raw_config = notebook.GetConfig();
  }

  if (session_config_updater_) {
    session_config_updater_();
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

Notebook *NotebookManager::FindNotebookByRootFolder(const std::string &root_folder) {
  for (const auto &pair : notebooks_) {
    if (pair.second->GetRootFolder() == root_folder) {
      return pair.second.get();
    }
  }
  return nullptr;
}

}  // namespace vxcore
