#include "bundled_notebook.h"

#include <filesystem>
#include <fstream>

#include "bundled_folder_manager.h"
#include "metadata_store.h"
#include "utils/logger.h"

namespace vxcore {

const char *BundledNotebook::kMetadataFolderName = "vx_notebook";

BundledNotebook::BundledNotebook(const std::string &local_data_folder,
                                 const std::string &root_folder)
    : Notebook(local_data_folder, root_folder, NotebookType::Bundled) {
  folder_manager_ = std::make_unique<BundledFolderManager>(this);
}

VxCoreError BundledNotebook::Create(const std::string &local_data_folder,
                                    const std::string &root_folder,
                                    const NotebookConfig *overridden_config,
                                    std::unique_ptr<Notebook> &out_notebook) {
  auto notebook =
      std::unique_ptr<BundledNotebook>(new BundledNotebook(local_data_folder, root_folder));
  if (overridden_config) {
    notebook->config_ = *overridden_config;
  }
  auto error = notebook->InitOnCreation();
  if (error != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to init bundled notebook on creation: root=%s, error=%d",
                     root_folder.c_str(), error);
    return error;
  }
  out_notebook = std::move(notebook);
  return VXCORE_OK;
}

VxCoreError BundledNotebook::InitOnCreation() {
  EnsureId();

  try {
    std::filesystem::path localDataPath(GetLocalDataFolder());
    std::filesystem::create_directories(localDataPath);

    std::filesystem::path metadataPath(GetMetadataFolder());
    std::filesystem::create_directories(metadataPath);
  } catch (const std::filesystem::filesystem_error &) {
    VXCORE_LOG_ERROR("Failed to create bundled notebook meta folders: root=%s",
                     root_folder_.c_str());
    return VXCORE_ERR_IO;
  }

  auto err = UpdateConfig(config_);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to save bundled notebook config: root=%s, error=%d",
                     root_folder_.c_str(), err);
    return err;
  }

  // Initialize MetadataStore
  err = InitMetadataStore();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to initialize MetadataStore: root=%s, error=%d", root_folder_.c_str(),
                     err);
    return err;
  }

  // Sync tags from NotebookConfig to MetadataStore
  err = SyncTagsToMetadataStore();
  if (err != VXCORE_OK) {
    VXCORE_LOG_WARN("Tag sync failed on creation: root=%s, error=%d", root_folder_.c_str(), err);
    // Continue anyway - tags will be synced on next open
  }

  err = folder_manager_->InitOnCreation();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to initialize bundled notebook folder manager: root=%s, error=%d",
                     root_folder_.c_str(), err);
  }
  return err;
}

VxCoreError BundledNotebook::Open(const std::string &local_data_folder,
                                  const std::string &root_folder,
                                  std::unique_ptr<Notebook> &out_notebook) {
  auto notebook =
      std::unique_ptr<BundledNotebook>(new BundledNotebook(local_data_folder, root_folder));
  auto err = notebook->LoadConfig();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to load bundled notebook config: root=%s, error=%d",
                     root_folder.c_str(), err);
    return err;
  }

  try {
    std::filesystem::path localDataPath(notebook->GetLocalDataFolder());
    std::filesystem::create_directories(localDataPath);

    std::filesystem::path metadataPath(notebook->GetMetadataFolder());
    std::filesystem::create_directories(metadataPath);
  } catch (const std::filesystem::filesystem_error &) {
    VXCORE_LOG_ERROR("Failed to create bundled notebook meta folders: root=%s",
                     notebook->root_folder_.c_str());
    return VXCORE_ERR_IO;
  }

  // Initialize MetadataStore
  err = notebook->InitMetadataStore();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to initialize MetadataStore on open: root=%s, error=%d",
                     root_folder.c_str(), err);
    return err;
  }

  // Sync tags from NotebookConfig to MetadataStore if needed
  err = notebook->SyncTagsToMetadataStore();
  if (err != VXCORE_OK) {
    VXCORE_LOG_WARN("Tag sync failed on open: root=%s, error=%d", root_folder.c_str(), err);
    // Continue anyway - tags will be synced on next open or RebuildCache
  }

  // Note: We do NOT sync folder/file MetadataStore from config files here.
  // The cache uses lazy sync - data is loaded on demand when accessed.
  // Users can call RebuildCache() if they need a full refresh.

  out_notebook = std::move(notebook);
  return VXCORE_OK;
}

std::string BundledNotebook::GetMetadataFolder() const {
  return ConcatenatePaths(root_folder_, kMetadataFolderName);
}

std::string BundledNotebook::GetConfigPath() const {
  return ConcatenatePaths(GetMetadataFolder(), kConfigFileName);
}

VxCoreError BundledNotebook::LoadConfig() {
  std::ifstream file(GetConfigPath());
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

VxCoreError BundledNotebook::UpdateConfig(const NotebookConfig &config) {
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

VxCoreError BundledNotebook::RebuildCache() {
  auto *bundled_folder_manager = dynamic_cast<BundledFolderManager *>(folder_manager_.get());
  if (!bundled_folder_manager) {
    VXCORE_LOG_ERROR("RebuildCache: folder_manager is not BundledFolderManager");
    return VXCORE_ERR_INVALID_STATE;
  }
  return bundled_folder_manager->SyncMetadataStoreFromConfigs();
}

std::string BundledNotebook::GetRecycleBinPath() const {
  auto *bundled_folder_manager = dynamic_cast<BundledFolderManager *>(folder_manager_.get());
  if (!bundled_folder_manager) {
    VXCORE_LOG_ERROR("GetRecycleBinPath: folder_manager is not BundledFolderManager");
    return "";
  }
  return bundled_folder_manager->GetRecycleBinPath();
}

VxCoreError BundledNotebook::EmptyRecycleBin() {
  std::string recycle_bin_path = GetRecycleBinPath();
  try {
    std::filesystem::path rb_path(recycle_bin_path);
    if (!std::filesystem::exists(rb_path)) {
      return VXCORE_OK;  // Nothing to empty
    }
    for (const auto &entry : std::filesystem::directory_iterator(rb_path)) {
      std::filesystem::remove_all(entry.path());
    }
    VXCORE_LOG_INFO("EmptyRecycleBin: Cleared recycle bin at %s", recycle_bin_path.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("EmptyRecycleBin: Failed to empty recycle bin: %s", e.what());
    return VXCORE_ERR_IO;
  }
}

}  // namespace vxcore
