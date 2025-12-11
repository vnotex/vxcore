#include "raw_notebook.h"

#include <filesystem>
#include <fstream>

#include "raw_folder_manager.h"
#include "utils/logger.h"

namespace vxcore {

RawNotebook::RawNotebook(const std::string &local_data_folder, const std::string &root_folder)
    : Notebook(local_data_folder, root_folder, NotebookType::Raw) {
  folder_manager_ = std::make_unique<RawFolderManager>(this);
}

VxCoreError RawNotebook::Create(const std::string &local_data_folder,
                                const std::string &root_folder,
                                const NotebookConfig *overridden_config,
                                std::unique_ptr<Notebook> &out_notebook) {
  auto notebook = std::unique_ptr<RawNotebook>(new RawNotebook(local_data_folder, root_folder));
  if (overridden_config) {
    notebook->config_ = *overridden_config;
  }
  auto err = notebook->InitOnCreation();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to init raw notebook on creation: root=%s, error=%d",
                     root_folder.c_str(), err);
    return err;
  }
  out_notebook = std::move(notebook);
  return VXCORE_OK;
}

VxCoreError RawNotebook::LoadConfig() {
  std::ifstream file(GetConfigPath());
  if (!file.is_open()) {
    return VXCORE_ERR_IO;
  }

  try {
    nlohmann::json json;
    file >> json;
    auto config = NotebookConfig::FromJson(json);
    if (config.id != config_.id) {
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

VxCoreError RawNotebook::InitOnCreation() {
  EnsureId();

  try {
    std::filesystem::path localDataPath(GetLocalDataFolder());
    std::filesystem::create_directories(localDataPath);
  } catch (const std::filesystem::filesystem_error &) {
    VXCORE_LOG_ERROR("Failed to create raw notebook meta folders: root=%s", root_folder_.c_str());
    return VXCORE_ERR_IO;
  }

  auto err = UpdateConfig(config_);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to save raw notebook config: root=%s, error=%d", root_folder_.c_str(),
                     err);
  }
  return err;
}

VxCoreError RawNotebook::Open(const std::string &local_data_folder, const std::string &root_folder,
                              const std::string &id, std::unique_ptr<Notebook> &out_notebook) {
  auto notebook = std::unique_ptr<RawNotebook>(new RawNotebook(local_data_folder, root_folder));
  notebook->config_.id = id;
  auto err = notebook->LoadConfig();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to load raw notebook config: root=%s, id=%s, error=%d",
                     root_folder.c_str(), id.c_str(), err);
    return err;
  }
  out_notebook = std::move(notebook);
  return VXCORE_OK;
}

std::string RawNotebook::GetMetadataFolder() const { return GetLocalDataFolder(); }

std::string RawNotebook::GetConfigPath() const {
  return ConcatenatePaths(GetMetadataFolder(), kConfigFileName);
}

VxCoreError RawNotebook::UpdateConfig(const NotebookConfig &config) {
  assert(config_.id == config.id);

  config_ = config;

  try {
    std::ofstream file(GetConfigPath());
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }

    nlohmann::json json = config_.ToJson();
    file << json.dump(2);
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

}  // namespace vxcore
