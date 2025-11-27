#include "folder_manager.h"

#include <filesystem>
#include <fstream>

#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

namespace fs = std::filesystem;

FolderManager::FolderManager(Notebook *notebook) : notebook_(notebook) { EnsureRootFolder(); }

FolderManager::~FolderManager() {}

void FolderManager::EnsureRootFolder() {
  if (notebook_->GetType() == NotebookType::Raw) {
    return;
  }

  const std::string folder_path("");
  std::unique_ptr<FolderConfig> root_config;
  VxCoreError error = LoadFolderConfig(folder_path, root_config);
  if (error != VXCORE_OK) {
    root_config.reset(new FolderConfig(folder_path));
    error = SaveFolderConfig(folder_path, *root_config);
    // TODO: log error if saving fails
  }
  CacheConfig(folder_path, std::move(root_config));
}

std::string FolderManager::GetConfigPath(const std::string &folder_path) const {
  if (notebook_->GetType() == NotebookType::Raw) {
    return "";
  }

  fs::path metadata_folder = notebook_->GetMetadataFolder();
  fs::path config_path = metadata_folder / "contents";

  if (!folder_path.empty() && folder_path != ".") {
    config_path /= folder_path;
  }

  config_path /= "vx.json";
  return config_path.string();
}

std::string FolderManager::GetContentPath(const std::string &folder_path) const {
  fs::path content_path = notebook_->GetRootFolder();

  if (!folder_path.empty() && folder_path != ".") {
    content_path /= folder_path;
  }

  return content_path.string();
}

FolderConfig *FolderManager::GetCachedConfig(const std::string &folder_path) {
  auto it = config_cache_.find(folder_path);
  if (it != config_cache_.end()) {
    return it->second.get();
  }
  return nullptr;
}

void FolderManager::CacheConfig(const std::string &folder_path,
                                std::unique_ptr<FolderConfig> config) {
  config_cache_[folder_path] = std::move(config);
}

void FolderManager::InvalidateCache(const std::string &folder_path) {
  config_cache_.erase(folder_path);
}

FileRecord *FolderManager::FindFileRecord(FolderConfig &config, const std::string &file_name) {
  for (auto &file : config.files) {
    if (file.name == file_name) {
      return &file;
    }
  }
  return nullptr;
}

VxCoreError FolderManager::LoadFolderConfig(const std::string &folder_path,
                                            std::unique_ptr<FolderConfig> &out_config) {
  out_config.reset();

  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  std::string config_path = GetConfigPath(folder_path);
  if (config_path.empty()) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  if (!fs::exists(config_path)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  std::ifstream file(config_path);
  if (!file.is_open()) {
    return VXCORE_ERR_IO;
  }

  try {
    nlohmann::json json;
    file >> json;
    out_config = std::make_unique<FolderConfig>(FolderConfig::FromJson(json));
    return VXCORE_OK;
  } catch (const std::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  }
}

VxCoreError FolderManager::SaveFolderConfig(const std::string &folder_path,
                                            const FolderConfig &config) {
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  std::string config_path = GetConfigPath(folder_path);
  if (config_path.empty()) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  fs::path config_file_path(config_path);
  fs::path config_dir = config_file_path.parent_path();

  try {
    if (!fs::exists(config_dir)) {
      fs::create_directories(config_dir);
    }

    std::ofstream file(config_path);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }

    nlohmann::json json = config.ToJson();
    file << json.dump(2);
    file.close();
    return VXCORE_OK;
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }
}

VxCoreError FolderManager::GetFolderConfig(const std::string &folder_path,
                                           std::string &out_config_json) {
  out_config_json.clear();
  FolderConfig *config = nullptr;
  VxCoreError error = GetFolderConfig(folder_path, &config);
  if (error != VXCORE_OK) {
    return error;
  }
  out_config_json = config->ToJson().dump();
  return VXCORE_OK;
}

VxCoreError FolderManager::GetFolderConfig(const std::string &folder_path,
                                           FolderConfig **out_config) {
  *out_config = nullptr;
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig *cached = GetCachedConfig(folder_path);
  if (cached) {
    *out_config = cached;
    return VXCORE_OK;
  }

  std::unique_ptr<FolderConfig> config;
  VxCoreError error = LoadFolderConfig(folder_path, config);
  if (error != VXCORE_OK) {
    return error;
  }

  *out_config = config.get();
  config_cache_[folder_path] = std::move(config);
  return VXCORE_OK;
}

VxCoreError FolderManager::CreateFolder(const std::string &parent_path,
                                        const std::string &folder_name,
                                        std::string &out_folder_id) {
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  VXCORE_LOG_INFO("Creating folder: parent=%s, name=%s", parent_path.c_str(), folder_name.c_str());

  std::string content_path = GetContentPath(parent_path);
  fs::path folder_path = fs::path(content_path) / folder_name;

  if (fs::exists(folder_path)) {
    VXCORE_LOG_WARN("Folder already exists: %s", folder_path.string().c_str());
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  FolderConfig *parent_config = nullptr;
  VxCoreError error = GetFolderConfig(parent_path, &parent_config);
  if (error != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to get parent folder config: parent=%s, error=%d", parent_path.c_str(),
                     error);
    return error;
  }

  auto it = std::find(parent_config->folders.begin(), parent_config->folders.end(), folder_name);
  if (it != parent_config->folders.end()) {
    VXCORE_LOG_WARN("Folder already exists in config: %s", folder_name.c_str());
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  try {
    fs::create_directories(folder_path);
    VXCORE_LOG_DEBUG("Created folder directory: %s", folder_path.string().c_str());
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to create folder directory: %s", e.what());
    return VXCORE_ERR_IO;
  }

  parent_config->folders.push_back(folder_name);
  parent_config->modified_utc = GetCurrentTimestampMillis();
  error = SaveFolderConfig(parent_path, *parent_config);
  if (error != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to save parent folder config: error=%d", error);
    return error;
  }

  const auto folder_relative_path = ConcatenatePaths(parent_path, folder_name);
  std::unique_ptr<FolderConfig> new_config(new FolderConfig(folder_name));
  error = SaveFolderConfig(folder_relative_path, *new_config);
  if (error != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to save new folder config: error=%d", error);
    return error;
  }

  out_folder_id = new_config->id;
  CacheConfig(folder_relative_path, std::move(new_config));
  VXCORE_LOG_INFO("Folder created successfully: id=%s", out_folder_id.c_str());
  return VXCORE_OK;
}

VxCoreError FolderManager::DeleteFolder(const std::string &folder_path) {
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  VXCORE_LOG_INFO("Deleting folder: path=%s", folder_path.c_str());

  const std::string content_path = GetContentPath(folder_path);
  const std::string config_path = GetConfigPath(folder_path);

  if (!fs::exists(content_path)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  InvalidateCache(folder_path);

  const auto [parent_path, folder_name] = SplitPath(folder_path);
  FolderConfig *parent_config = nullptr;
  VxCoreError error = GetFolderConfig(parent_path, &parent_config);
  if (error != VXCORE_OK) {
    return error;
  }
  auto it = std::find(parent_config->folders.begin(), parent_config->folders.end(), folder_name);
  if (it != parent_config->folders.end()) {
    parent_config->folders.erase(it);
    parent_config->modified_utc = GetCurrentTimestampMillis();
    error = SaveFolderConfig(parent_path, *parent_config);
    if (error != VXCORE_OK) {
      return error;
    }
  }

  try {
    if (fs::exists(config_path)) {
      fs::remove_all(fs::path(config_path).parent_path());
    }
    fs::remove_all(content_path);
    VXCORE_LOG_INFO("Folder deleted successfully: path=%s", folder_path.c_str());
    return VXCORE_OK;
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }
}

VxCoreError FolderManager::UpdateFolderMetadata(const std::string &folder_path,
                                                const std::string &metadata_json) {
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig *config = nullptr;
  VxCoreError error = GetFolderConfig(folder_path, &config);
  if (error != VXCORE_OK) {
    return error;
  }

  try {
    nlohmann::json metadata = nlohmann::json::parse(metadata_json);
    if (!metadata.is_object()) {
      return VXCORE_ERR_JSON_PARSE;
    }

    config->metadata = metadata;
    config->modified_utc = GetCurrentTimestampMillis();

    error = SaveFolderConfig(folder_path, *config);
    return error;
  } catch (const std::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  }
}

VxCoreError FolderManager::GetFolderMetadata(const std::string &folder_path,
                                             std::string &out_metadata_json) {
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig *config = nullptr;
  VxCoreError error = GetFolderConfig(folder_path, &config);
  if (error != VXCORE_OK) {
    return error;
  }

  out_metadata_json = config->metadata.dump();
  return VXCORE_OK;
}

VxCoreError FolderManager::RenameFolder(const std::string &folder_path,
                                        const std::string &new_name) {
  VXCORE_LOG_INFO("RenameFolder: folder_path=%s, new_name=%s", folder_path.c_str(),
                  new_name.c_str());

  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  const auto [parent_path, old_name] = SplitPath(folder_path);
  const std::string old_content_path = GetContentPath(folder_path);
  const std::string old_config_path = GetConfigPath(folder_path);

  if (!fs::exists(old_content_path)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  const std::string new_folder_path = ConcatenatePaths(parent_path, new_name);
  const std::string new_content_path = GetContentPath(new_folder_path);
  const std::string new_config_path = GetConfigPath(new_folder_path);

  if (fs::exists(new_content_path)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  FolderConfig *parent_config = nullptr;
  VxCoreError error = GetFolderConfig(parent_path, &parent_config);
  if (error != VXCORE_OK) {
    return error;
  }

  auto it = std::find(parent_config->folders.begin(), parent_config->folders.end(), old_name);
  if (it == parent_config->folders.end()) {
    return VXCORE_ERR_NOT_FOUND;
  }

  FolderConfig *folder_config = nullptr;
  error = GetFolderConfig(folder_path, &folder_config);
  if (error != VXCORE_OK) {
    return error;
  }

  try {
    fs::rename(old_content_path, new_content_path);

    if (fs::exists(old_config_path)) {
      fs::create_directories(fs::path(new_config_path).parent_path());
      fs::rename(old_config_path, new_config_path);
      fs::remove_all(fs::path(old_config_path).parent_path());
    }
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  folder_config->name = new_name;
  folder_config->modified_utc = GetCurrentTimestampMillis();
  error = SaveFolderConfig(new_folder_path, *folder_config);
  if (error != VXCORE_OK) {
    return error;
  }

  *it = new_name;
  parent_config->modified_utc = GetCurrentTimestampMillis();
  error = SaveFolderConfig(parent_path, *parent_config);
  if (error != VXCORE_OK) {
    return error;
  }

  InvalidateCache(folder_path);
  InvalidateCache(new_folder_path);

  VXCORE_LOG_INFO("RenameFolder successful: folder renamed from %s to %s", folder_path.c_str(),
                  new_folder_path.c_str());
  return VXCORE_OK;
}

VxCoreError FolderManager::MoveFolder(const std::string &src_path,
                                      const std::string &dest_parent_path) {
  VXCORE_LOG_INFO("MoveFolder: src_path=%s, dest_parent_path=%s", src_path.c_str(),
                  dest_parent_path.c_str());

  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  const auto [src_parent_path, folder_name] = SplitPath(src_path);
  const std::string src_content_path = GetContentPath(src_path);
  const std::string src_config_path = GetConfigPath(src_path);

  if (!fs::exists(src_content_path)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  const std::string dest_path = ConcatenatePaths(dest_parent_path, folder_name);
  const std::string dest_content_path = GetContentPath(dest_path);
  const std::string dest_config_path = GetConfigPath(dest_path);

  if (fs::exists(dest_content_path)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  FolderConfig *src_parent_config = nullptr;
  VxCoreError error = GetFolderConfig(src_parent_path, &src_parent_config);
  if (error != VXCORE_OK) {
    return error;
  }

  FolderConfig *dest_parent_config = nullptr;
  error = GetFolderConfig(dest_parent_path, &dest_parent_config);
  if (error != VXCORE_OK) {
    return error;
  }

  auto it =
      std::find(src_parent_config->folders.begin(), src_parent_config->folders.end(), folder_name);
  if (it == src_parent_config->folders.end()) {
    return VXCORE_ERR_NOT_FOUND;
  }

  auto dest_it = std::find(dest_parent_config->folders.begin(), dest_parent_config->folders.end(),
                           folder_name);
  if (dest_it != dest_parent_config->folders.end()) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  try {
    fs::create_directories(fs::path(dest_content_path).parent_path());
    fs::rename(src_content_path, dest_content_path);

    if (fs::exists(src_config_path)) {
      const fs::path src_config_dir = fs::path(src_config_path).parent_path();
      const fs::path dest_config_dir = fs::path(dest_config_path).parent_path();
      fs::create_directories(dest_config_dir.parent_path());
      fs::rename(src_config_dir, dest_config_dir);
    }
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  src_parent_config->folders.erase(it);
  src_parent_config->modified_utc = GetCurrentTimestampMillis();
  error = SaveFolderConfig(src_parent_path, *src_parent_config);
  if (error != VXCORE_OK) {
    return error;
  }

  dest_parent_config->folders.push_back(folder_name);
  dest_parent_config->modified_utc = GetCurrentTimestampMillis();
  error = SaveFolderConfig(dest_parent_path, *dest_parent_config);
  if (error != VXCORE_OK) {
    return error;
  }

  InvalidateCache(src_path);
  InvalidateCache(dest_path);

  VXCORE_LOG_INFO("MoveFolder successful: folder moved from %s to %s", src_path.c_str(),
                  dest_path.c_str());
  return VXCORE_OK;
}

VxCoreError FolderManager::CopyFolder(const std::string &src_path,
                                      const std::string &dest_parent_path,
                                      const std::string &new_name, std::string &out_folder_id) {
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  const auto [src_parent_path, src_folder_name] = SplitPath(src_path);
  const std::string src_content_path = GetContentPath(src_path);
  const std::string folder_name = new_name.empty() ? src_folder_name : new_name;

  VXCORE_LOG_INFO("Copying folder: src=%s, dest=%s, new_name=%s", src_path.c_str(),
                  dest_parent_path.c_str(), folder_name.c_str());

  if (!fs::exists(src_content_path)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  const std::string dest_path = ConcatenatePaths(dest_parent_path, folder_name);
  const std::string dest_content_path = GetContentPath(dest_path);

  if (fs::exists(dest_content_path)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  FolderConfig *dest_parent_config = nullptr;
  VxCoreError error = GetFolderConfig(dest_parent_path, &dest_parent_config);
  if (error != VXCORE_OK) {
    return error;
  }

  auto dest_it = std::find(dest_parent_config->folders.begin(), dest_parent_config->folders.end(),
                           folder_name);
  if (dest_it != dest_parent_config->folders.end()) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  FolderConfig *src_config = nullptr;
  error = GetFolderConfig(src_path, &src_config);
  if (error != VXCORE_OK) {
    return error;
  }

  try {
    fs::copy(src_content_path, dest_content_path, fs::copy_options::recursive);
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  std::unique_ptr<FolderConfig> dest_config = std::make_unique<FolderConfig>(*src_config);
  dest_config->id = GenerateUUID();
  dest_config->name = folder_name;
  dest_config->created_utc = GetCurrentTimestampMillis();
  dest_config->modified_utc = dest_config->created_utc;

  for (auto &file : dest_config->files) {
    file.id = GenerateUUID();
    file.created_utc = dest_config->created_utc;
    file.modified_utc = dest_config->created_utc;
  }

  out_folder_id = dest_config->id;

  error = SaveFolderConfig(dest_path, *dest_config);
  if (error != VXCORE_OK) {
    return error;
  }

  dest_parent_config->folders.push_back(folder_name);
  dest_parent_config->modified_utc = GetCurrentTimestampMillis();
  error = SaveFolderConfig(dest_parent_path, *dest_parent_config);
  if (error != VXCORE_OK) {
    return error;
  }

  CacheConfig(dest_path, std::move(dest_config));

  VXCORE_LOG_INFO("Folder copied successfully: id=%s", out_folder_id.c_str());
  return VXCORE_OK;
}

VxCoreError FolderManager::CreateFile(const std::string &folder_path, const std::string &file_name,
                                      std::string &out_file_id) {
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  VXCORE_LOG_INFO("Creating file: folder=%s, name=%s", folder_path.c_str(), file_name.c_str());

  std::string content_path = GetContentPath(folder_path);
  fs::path file_path = fs::path(content_path) / file_name;

  if (fs::exists(file_path)) {
    VXCORE_LOG_WARN("File already exists: %s", file_path.string().c_str());
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  FolderConfig *config = nullptr;
  VxCoreError error = GetFolderConfig(folder_path, &config);
  if (error != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to get folder config: folder=%s, error=%d", folder_path.c_str(),
                     error);
    return error;
  }

  if (FindFileRecord(*config, file_name)) {
    VXCORE_LOG_WARN("File already exists in config: %s", file_name.c_str());
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  try {
    std::ofstream(file_path).close();
    VXCORE_LOG_DEBUG("Created file: %s", file_path.string().c_str());
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to create file: %s", e.what());
    return VXCORE_ERR_IO;
  }

  const auto ts = GetCurrentTimestampMillis();
  config->files.emplace_back(file_name);
  config->files.back().created_utc = ts;
  config->files.back().modified_utc = ts;
  config->modified_utc = ts;

  error = SaveFolderConfig(folder_path, *config);
  if (error != VXCORE_OK) {
    return error;
  }

  out_file_id = config->files.back().id;
  return VXCORE_OK;
}

VxCoreError FolderManager::DeleteFile(const std::string &folder_path,
                                      const std::string &file_name) {
  VXCORE_LOG_INFO("DeleteFile: folder_path=%s, file_name=%s", folder_path.c_str(),
                  file_name.c_str());

  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig *config = nullptr;
  VxCoreError error = GetFolderConfig(folder_path, &config);
  if (error != VXCORE_OK) {
    return error;
  }

  auto it = std::find_if(config->files.begin(), config->files.end(),
                         [&file_name](const FileRecord &file) { return file.name == file_name; });

  if (it == config->files.end()) {
    return VXCORE_ERR_NOT_FOUND;
  }

  config->files.erase(it);
  config->modified_utc = GetCurrentTimestampMillis();
  error = SaveFolderConfig(folder_path, *config);
  if (error != VXCORE_OK) {
    return error;
  }

  try {
    std::string content_path = GetContentPath(folder_path);
    fs::path file_path = fs::path(content_path) / file_name;
    fs::remove(file_path);
    VXCORE_LOG_INFO("DeleteFile successful: file %s deleted from folder %s", file_name.c_str(),
                    folder_path.c_str());
    return VXCORE_OK;
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }
}

VxCoreError FolderManager::UpdateFileMetadata(const std::string &folder_path,
                                              const std::string &file_name,
                                              const std::string &metadata_json) {
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig *config = nullptr;
  VxCoreError error = GetFolderConfig(folder_path, &config);
  if (error != VXCORE_OK) {
    return error;
  }

  FileRecord *file = FindFileRecord(*config, file_name);
  if (!file) {
    return VXCORE_ERR_NOT_FOUND;
  }

  try {
    nlohmann::json metadata = nlohmann::json::parse(metadata_json);
    if (!metadata.is_object()) {
      return VXCORE_ERR_JSON_PARSE;
    }

    file->metadata = metadata;
    file->modified_utc = GetCurrentTimestampMillis();

    config->modified_utc = file->modified_utc;

    error = SaveFolderConfig(folder_path, *config);
    return error;
  } catch (const std::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  }
}

VxCoreError FolderManager::UpdateFileTags(const std::string &folder_path,
                                          const std::string &file_name,
                                          const std::string &tags_json) {
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig *config = nullptr;
  VxCoreError error = GetFolderConfig(folder_path, &config);
  if (error != VXCORE_OK) {
    return error;
  }

  FileRecord *file = FindFileRecord(*config, file_name);
  if (!file) {
    return VXCORE_ERR_NOT_FOUND;
  }

  try {
    nlohmann::json tags = nlohmann::json::parse(tags_json);
    if (!tags.is_array()) {
      return VXCORE_ERR_JSON_PARSE;
    }

    file->tags = tags.get<std::vector<std::string>>();
    file->modified_utc = GetCurrentTimestampMillis();

    config->modified_utc = file->modified_utc;

    error = SaveFolderConfig(folder_path, *config);
    return error;
  } catch (const std::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  }
}

VxCoreError FolderManager::GetFileInfo(const std::string &folder_path, const std::string &file_name,
                                       std::string &out_file_info_json) {
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig *config = nullptr;
  VxCoreError error = GetFolderConfig(folder_path, &config);
  if (error != VXCORE_OK) {
    return error;
  }

  FileRecord *file = FindFileRecord(*config, file_name);
  if (!file) {
    return VXCORE_ERR_NOT_FOUND;
  }

  out_file_info_json = file->ToJson().dump();
  return VXCORE_OK;
}

VxCoreError FolderManager::GetFileMetadata(const std::string &folder_path,
                                           const std::string &file_name,
                                           std::string &out_metadata_json) {
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig *config = nullptr;
  VxCoreError error = GetFolderConfig(folder_path, &config);
  if (error != VXCORE_OK) {
    return error;
  }

  FileRecord *file = FindFileRecord(*config, file_name);
  if (!file) {
    return VXCORE_ERR_NOT_FOUND;
  }

  out_metadata_json = file->metadata.dump();
  return VXCORE_OK;
}

VxCoreError FolderManager::RenameFile(const std::string &folder_path, const std::string &old_name,
                                      const std::string &new_name) {
  VXCORE_LOG_INFO("RenameFile: folder_path=%s, old_name=%s, new_name=%s", folder_path.c_str(),
                  old_name.c_str(), new_name.c_str());

  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig *config = nullptr;
  VxCoreError error = GetFolderConfig(folder_path, &config);
  if (error != VXCORE_OK) {
    return error;
  }

  FileRecord *file = FindFileRecord(*config, old_name);
  if (!file) {
    return VXCORE_ERR_NOT_FOUND;
  }

  FileRecord *existing_file = FindFileRecord(*config, new_name);
  if (existing_file) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  const std::string content_path = GetContentPath(folder_path);
  fs::path old_file_path = fs::path(content_path) / old_name;
  fs::path new_file_path = fs::path(content_path) / new_name;

  if (!fs::exists(old_file_path)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  if (fs::exists(new_file_path)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  try {
    fs::rename(old_file_path, new_file_path);
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  file->name = new_name;
  file->modified_utc = GetCurrentTimestampMillis();
  config->modified_utc = file->modified_utc;

  error = SaveFolderConfig(folder_path, *config);
  if (error == VXCORE_OK) {
    VXCORE_LOG_INFO("RenameFile successful: file renamed from %s to %s in folder %s",
                    old_name.c_str(), new_name.c_str(), folder_path.c_str());
  }
  return error;
}

VxCoreError FolderManager::MoveFile(const std::string &src_folder_path,
                                    const std::string &file_name,
                                    const std::string &dest_folder_path) {
  VXCORE_LOG_INFO("MoveFile: src_folder_path=%s, file_name=%s, dest_folder_path=%s",
                  src_folder_path.c_str(), file_name.c_str(), dest_folder_path.c_str());

  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig *src_config = nullptr;
  VxCoreError error = GetFolderConfig(src_folder_path, &src_config);
  if (error != VXCORE_OK) {
    return error;
  }

  FileRecord *file = FindFileRecord(*src_config, file_name);
  if (!file) {
    return VXCORE_ERR_NOT_FOUND;
  }

  FolderConfig *dest_config = nullptr;
  error = GetFolderConfig(dest_folder_path, &dest_config);
  if (error != VXCORE_OK) {
    return error;
  }

  FileRecord *existing_file = FindFileRecord(*dest_config, file_name);
  if (existing_file) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  const std::string src_content_path = GetContentPath(src_folder_path);
  const std::string dest_content_path = GetContentPath(dest_folder_path);
  fs::path src_file_path = fs::path(src_content_path) / file_name;
  fs::path dest_file_path = fs::path(dest_content_path) / file_name;

  if (!fs::exists(src_file_path)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  if (fs::exists(dest_file_path)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  try {
    fs::rename(src_file_path, dest_file_path);
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  FileRecord file_copy = *file;
  file_copy.modified_utc = GetCurrentTimestampMillis();

  auto it = std::find_if(src_config->files.begin(), src_config->files.end(),
                         [&file_name](const FileRecord &f) { return f.name == file_name; });
  src_config->files.erase(it);
  src_config->modified_utc = file_copy.modified_utc;
  error = SaveFolderConfig(src_folder_path, *src_config);
  if (error != VXCORE_OK) {
    return error;
  }

  dest_config->files.push_back(file_copy);
  dest_config->modified_utc = file_copy.modified_utc;
  error = SaveFolderConfig(dest_folder_path, *dest_config);
  if (error == VXCORE_OK) {
    VXCORE_LOG_INFO("MoveFile successful: file %s moved from %s to %s", file_name.c_str(),
                    src_folder_path.c_str(), dest_folder_path.c_str());
  }
  return error;
}

VxCoreError FolderManager::CopyFile(const std::string &src_folder_path,
                                    const std::string &file_name,
                                    const std::string &dest_folder_path,
                                    const std::string &new_name, std::string &out_file_id) {
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  const std::string target_name = new_name.empty() ? file_name : new_name;
  VXCORE_LOG_INFO("Copying file: src=%s, file=%s, dest=%s, new_name=%s", src_folder_path.c_str(),
                  file_name.c_str(), dest_folder_path.c_str(), target_name.c_str());

  FolderConfig *src_config = nullptr;
  VxCoreError error = GetFolderConfig(src_folder_path, &src_config);
  if (error != VXCORE_OK) {
    return error;
  }

  FileRecord *file = FindFileRecord(*src_config, file_name);
  if (!file) {
    return VXCORE_ERR_NOT_FOUND;
  }

  FolderConfig *dest_config = nullptr;
  error = GetFolderConfig(dest_folder_path, &dest_config);
  if (error != VXCORE_OK) {
    return error;
  }

  FileRecord *existing_file = FindFileRecord(*dest_config, target_name);
  if (existing_file) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  const std::string src_content_path = GetContentPath(src_folder_path);
  const std::string dest_content_path = GetContentPath(dest_folder_path);
  fs::path src_file_path = fs::path(src_content_path) / file_name;
  fs::path dest_file_path = fs::path(dest_content_path) / target_name;

  if (!fs::exists(src_file_path)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  if (fs::exists(dest_file_path)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  try {
    fs::copy_file(src_file_path, dest_file_path);
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  FileRecord new_file = *file;
  new_file.id = GenerateUUID();
  new_file.name = target_name;
  new_file.created_utc = GetCurrentTimestampMillis();
  new_file.modified_utc = new_file.created_utc;

  out_file_id = new_file.id;

  dest_config->files.push_back(new_file);
  dest_config->modified_utc = new_file.created_utc;
  error = SaveFolderConfig(dest_folder_path, *dest_config);
  if (error == VXCORE_OK) {
    VXCORE_LOG_INFO("File copied successfully: id=%s", out_file_id.c_str());
  }
  return error;
}

void FolderManager::ClearCache() { config_cache_.clear(); }

std::string FolderManager::ConcatenatePaths(const std::string &parent_path,
                                            const std::string &child_name) const {
  if (parent_path.empty() || parent_path == ".") {
    return child_name;
  } else {
    return parent_path + "/" + child_name;
  }
}

std::pair<std::string, std::string> FolderManager::SplitPath(const std::string &path) const {
  size_t last_slash = path.find_last_of("/\\");
  if (last_slash == std::string::npos) {
    return {".", path};
  } else {
    std::string parent_path = path.substr(0, last_slash);
    std::string child_name = path.substr(last_slash + 1);
    return {parent_path, child_name};
  }
}

}  // namespace vxcore
