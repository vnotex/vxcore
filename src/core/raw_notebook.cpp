#include "raw_notebook.h"

#include <filesystem>

#include "metadata_store.h"
#include "raw_folder_manager.h"
#include "utils/file_utils.h"
#include "utils/logger.h"

namespace vxcore {

namespace {
const char *kMetaKeyName = "config.name";
const char *kMetaKeyDescription = "config.description";
const char *kMetaKeyAssetsFolder = "config.assetsFolder";
const char *kMetaKeyMetadata = "config.metadata";
const char *kMetaKeyIgnored = "config.ignored";
}  // namespace

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
  if (!metadata_store_ || !metadata_store_->IsOpen()) {
    return VXCORE_ERR_INVALID_STATE;
  }

  auto name = metadata_store_->GetNotebookMetadata(kMetaKeyName);
  if (name.has_value()) {
    config_.name = name.value();
  }

  auto desc = metadata_store_->GetNotebookMetadata(kMetaKeyDescription);
  if (desc.has_value()) {
    config_.description = desc.value();
  }

  auto assets = metadata_store_->GetNotebookMetadata(kMetaKeyAssetsFolder);
  if (assets.has_value()) {
    config_.assets_folder = assets.value();
  }

  auto meta = metadata_store_->GetNotebookMetadata(kMetaKeyMetadata);
  if (meta.has_value()) {
    try {
      config_.metadata = nlohmann::json::parse(meta.value());
    } catch (const nlohmann::json::exception &) {
      VXCORE_LOG_WARN("Failed to parse metadata JSON for raw notebook: id=%s",
                      config_.id.c_str());
    }
  }

  auto ignored_str = metadata_store_->GetNotebookMetadata(kMetaKeyIgnored);
  if (ignored_str.has_value()) {
    try {
      auto arr = nlohmann::json::parse(ignored_str.value());
      if (arr.is_array()) {
        for (const auto &item : arr) {
          if (item.is_string()) {
            config_.ignored.push_back(item.get<std::string>());
          }
        }
      }
    } catch (const nlohmann::json::exception &) {
      VXCORE_LOG_WARN("Failed to parse ignored JSON for raw notebook: id=%s",
                      config_.id.c_str());
    }
  }

  return VXCORE_OK;
}

VxCoreError RawNotebook::SaveConfigToDb() {
  if (!metadata_store_ || !metadata_store_->IsOpen()) {
    return VXCORE_ERR_INVALID_STATE;
  }

  if (!metadata_store_->SetNotebookMetadata(kMetaKeyName, config_.name)) {
    return VXCORE_ERR_DATABASE;
  }
  if (!metadata_store_->SetNotebookMetadata(kMetaKeyDescription, config_.description)) {
    return VXCORE_ERR_DATABASE;
  }
  if (!metadata_store_->SetNotebookMetadata(kMetaKeyAssetsFolder, config_.assets_folder)) {
    return VXCORE_ERR_DATABASE;
  }
  if (!metadata_store_->SetNotebookMetadata(kMetaKeyMetadata, config_.metadata.dump())) {
    return VXCORE_ERR_DATABASE;
  }
  if (!metadata_store_->SetNotebookMetadata(kMetaKeyIgnored,
                                             nlohmann::json(config_.ignored).dump())) {
    return VXCORE_ERR_DATABASE;
  }

  return VXCORE_OK;
}

VxCoreError RawNotebook::InitOnCreation() {
  EnsureId();

  try {
    auto localDataPath = PathFromUtf8(GetLocalDataFolder());
    std::filesystem::create_directories(localDataPath);
  } catch (const std::filesystem::filesystem_error &) {
    VXCORE_LOG_ERROR("Failed to create raw notebook meta folders: root=%s", root_folder_.c_str());
    return VXCORE_ERR_IO;
  }

  // Initialize MetadataStore first (needed for DB config writes)
  auto err = InitMetadataStore();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to initialize MetadataStore: root=%s, error=%d", root_folder_.c_str(),
                     err);
    return err;
  }

  // Write config fields to DB
  err = SaveConfigToDb();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to save raw notebook config to DB: root=%s, error=%d",
                     root_folder_.c_str(), err);
    return err;
  }

  return VXCORE_OK;
}

VxCoreError RawNotebook::Open(const std::string &local_data_folder, const std::string &root_folder,
                              const std::string &id, std::unique_ptr<Notebook> &out_notebook) {
  auto notebook = std::unique_ptr<RawNotebook>(new RawNotebook(local_data_folder, root_folder));
  notebook->config_.id = id;

  // Initialize MetadataStore first (needed to read config from DB)
  auto err = notebook->InitMetadataStore();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to initialize MetadataStore on open: root=%s, id=%s, error=%d",
                     root_folder.c_str(), id.c_str(), err);
    return err;
  }

  // Read config from DB
  err = notebook->LoadConfig();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to load raw notebook config from DB: root=%s, id=%s, error=%d",
                     root_folder.c_str(), id.c_str(), err);
    return err;
  }

  out_notebook = std::move(notebook);
  return VXCORE_OK;
}

std::string RawNotebook::GetMetadataFolder() const { return GetLocalDataFolder(); }

std::string RawNotebook::GetConfigPath() const { return ""; }

VxCoreError RawNotebook::UpdateConfig(const NotebookConfig &config) {
  assert(config_.id == config.id);
  config_ = config;
  return SaveConfigToDb();
}

VxCoreError RawNotebook::RebuildCache() {
  // Raw notebooks don't have vx.json config files to sync from.
  // Metadata is stored only in the database.
  return VXCORE_OK;
}

std::string RawNotebook::GetRecycleBinPath() const {
  // Raw notebooks do not support recycle bin
  return "";
}

VxCoreError RawNotebook::EmptyRecycleBin() {
  // Raw notebooks do not support recycle bin
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawNotebook::CreateTag(const std::string &tag_name, const std::string &parent_tag) {
  (void)tag_name;
  (void)parent_tag;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawNotebook::CreateTagPath(const std::string &tag_path) {
  (void)tag_path;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawNotebook::DeleteTag(const std::string &tag_name) {
  (void)tag_name;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawNotebook::MoveTag(const std::string &tag_name, const std::string &parent_tag) {
  (void)tag_name;
  (void)parent_tag;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawNotebook::GetTags(std::string &out_tags_json) const {
  out_tags_json = "[]";
  return VXCORE_OK;
}

VxCoreError RawNotebook::FindFilesByTags(const std::vector<std::string> &tags, bool use_and,
                                         std::string &out_results_json) {
  (void)tags;
  (void)use_and;
  (void)out_results_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawNotebook::CountFilesByTag(std::string &out_results_json) {
  (void)out_results_json;
  return VXCORE_ERR_UNSUPPORTED;
}

}  // namespace vxcore
