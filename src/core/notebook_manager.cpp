#include "notebook_manager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace vxcore {

NotebookManager::NotebookManager(const std::string &localDataFolder,
                                 VxCoreSessionConfig *sessionConfig)
    : localDataFolder_(localDataFolder), sessionConfig_(sessionConfig) {}

NotebookManager::~NotebookManager() {}

VxCoreError NotebookManager::createNotebook(const std::string &rootFolder, NotebookType type,
                                            const std::string &propertiesJson,
                                            std::string &outNotebookId) {
  std::lock_guard<std::mutex> lock(mutex_);

  try {
    std::filesystem::path rootPath(rootFolder);
    if (!std::filesystem::exists(rootPath)) {
      std::filesystem::create_directories(rootPath);
    }

    NotebookConfig config;
    if (!propertiesJson.empty()) {
      nlohmann::json json = nlohmann::json::parse(propertiesJson);
      config = NotebookConfig::fromJson(json);
    }

    auto notebook = std::make_unique<Notebook>(rootFolder, type, config);

    VxCoreError err = notebook->saveConfig();
    if (err != VXCORE_OK) {
      return err;
    }

    outNotebookId = notebook->getId();

    NotebookRecord record;
    record.id = notebook->getId();
    record.rootFolder = rootFolder;
    record.type = type;
    record.lastOpenedTimestamp =
        std::chrono::system_clock::now().time_since_epoch().count() / 1000000;
    if (type == NotebookType::Raw) {
      record.rawConfig = config;
    }

    err = saveNotebookRecord(record);
    if (err != VXCORE_OK) {
      return err;
    }

    notebooks_[outNotebookId] = std::move(notebook);

    updateSessionConfig();

    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::filesystem::filesystem_error &) {
    return VXCORE_ERR_IO;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::openNotebook(const std::string &rootFolder,
                                          std::string &outNotebookId) {
  std::lock_guard<std::mutex> lock(mutex_);

  try {
    std::filesystem::path rootPath(rootFolder);
    if (!std::filesystem::exists(rootPath)) {
      return VXCORE_ERR_NOT_FOUND;
    }

    NotebookRecord record;
    VxCoreError err = loadNotebookRecord(rootFolder, record);
    if (err != VXCORE_OK) {
      return err;
    }

    for (const auto &pair : notebooks_) {
      if (pair.second->getRootFolder() == rootFolder) {
        outNotebookId = pair.first;
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
      config = NotebookConfig::fromJson(json);
    } else {
      config = record.rawConfig;
    }

    auto notebook = std::make_unique<Notebook>(rootFolder, type, config);
    outNotebookId = notebook->getId();

    record.lastOpenedTimestamp =
        std::chrono::system_clock::now().time_since_epoch().count() / 1000000;
    err = saveNotebookRecord(record);
    if (err != VXCORE_OK) {
      return err;
    }

    notebooks_[outNotebookId] = std::move(notebook);

    updateSessionConfig();

    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::filesystem::filesystem_error &) {
    return VXCORE_ERR_IO;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::closeNotebook(const std::string &notebookId) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = notebooks_.find(notebookId);
  if (it == notebooks_.end()) {
    return VXCORE_ERR_NOT_FOUND;
  }

  notebooks_.erase(it);

  updateSessionConfig();

  return VXCORE_OK;
}

VxCoreError NotebookManager::getNotebookProperties(const std::string &notebookId,
                                                    std::string &outPropertiesJson) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = notebooks_.find(notebookId);
  if (it == notebooks_.end()) {
    return VXCORE_ERR_NOT_FOUND;
  }

  try {
    nlohmann::json json = it->second->getConfig().toJson();
    json["rootFolder"] = it->second->getRootFolder();
    json["type"] = (it->second->getType() == NotebookType::Raw) ? "raw" : "bundled";
    outPropertiesJson = json.dump();
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::setNotebookProperties(const std::string &notebookId,
                                                    const std::string &propertiesJson) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = notebooks_.find(notebookId);
  if (it == notebooks_.end()) {
    return VXCORE_ERR_NOT_FOUND;
  }

  try {
    nlohmann::json json = nlohmann::json::parse(propertiesJson);
    NotebookConfig config = NotebookConfig::fromJson(json);
    config.id = notebookId;

    it->second->setConfig(config);

    VxCoreError err = it->second->saveConfig();
    if (err != VXCORE_OK) {
      return err;
    }

    NotebookRecord record;
    record.id = notebookId;
    record.rootFolder = it->second->getRootFolder();
    record.type = it->second->getType();
    record.lastOpenedTimestamp =
        std::chrono::system_clock::now().time_since_epoch().count() / 1000000;
    if (record.type == NotebookType::Raw) {
      record.rawConfig = config;
    }

    err = saveNotebookRecord(record);
    if (err != VXCORE_OK) {
      return err;
    }

    updateSessionConfig();

    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::listNotebooks(std::string &outNotebooksJson) {
  std::lock_guard<std::mutex> lock(mutex_);

  try {
    nlohmann::json jsonArray = nlohmann::json::array();

    for (const auto &pair : notebooks_) {
      nlohmann::json item = nlohmann::json::object();
      item["id"] = pair.second->getId();
      item["rootFolder"] = pair.second->getRootFolder();
      item["type"] = (pair.second->getType() == NotebookType::Raw) ? "raw" : "bundled";
      item["config"] = pair.second->getConfig().toJson();
      jsonArray.push_back(item);
    }

    outNotebooksJson = jsonArray.dump();
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

Notebook *NotebookManager::getNotebook(const std::string &notebookId) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = notebooks_.find(notebookId);
  if (it == notebooks_.end()) {
    return nullptr;
  }
  return it->second.get();
}

void NotebookManager::setSessionConfigUpdater(std::function<void()> updater) {
  sessionConfigUpdater_ = std::move(updater);
}

VxCoreError NotebookManager::loadNotebookRecord(const std::string &rootFolder,
                                                 NotebookRecord &record) {
  if (sessionConfig_) {
    for (const auto &existingRecord : sessionConfig_->notebooks) {
      if (existingRecord.rootFolder == rootFolder) {
        record = existingRecord;
        return VXCORE_OK;
      }
    }
  }

  std::filesystem::path rootPath(rootFolder);
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
      NotebookConfig config = NotebookConfig::fromJson(json);

      record.id = config.id;
      record.rootFolder = rootFolder;
      record.type = NotebookType::Bundled;
      record.lastOpenedTimestamp = 0;
      return VXCORE_OK;
    } catch (const nlohmann::json::exception &) {
      return VXCORE_ERR_JSON_PARSE;
    }
  } else {
    return VXCORE_ERR_NOT_FOUND;
  }
}

VxCoreError NotebookManager::saveNotebookRecord(const NotebookRecord &record) {
  if (!sessionConfig_) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  NotebookRecord *existing = findNotebookRecord(record.id);
  if (existing) {
    *existing = record;
  } else {
    sessionConfig_->notebooks.push_back(record);
  }

  return VXCORE_OK;
}

NotebookRecord *NotebookManager::findNotebookRecord(const std::string &id) {
  if (!sessionConfig_) {
    return nullptr;
  }

  auto it = std::find_if(sessionConfig_->notebooks.begin(), sessionConfig_->notebooks.end(),
                         [&id](const NotebookRecord &r) { return r.id == id; });

  if (it != sessionConfig_->notebooks.end()) {
    return &(*it);
  }

  return nullptr;
}

void NotebookManager::updateSessionConfig() {
  if (sessionConfigUpdater_) {
    sessionConfigUpdater_();
  }
}

} // namespace vxcore
