#include "folder_manager.h"

#include <filesystem>
#include <fstream>

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

  std::string content_path = GetContentPath(parent_path);
  fs::path folder_path = fs::path(content_path) / folder_name;

  if (fs::exists(folder_path)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  FolderConfig *parent_config = nullptr;
  VxCoreError error = GetFolderConfig(parent_path, &parent_config);
  if (error != VXCORE_OK) {
    return error;
  }

  auto it = std::find(parent_config->folders.begin(), parent_config->folders.end(), folder_name);
  if (it != parent_config->folders.end()) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  try {
    fs::create_directories(folder_path);
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  parent_config->folders.push_back(folder_name);
  parent_config->modified_utc = GetCurrentTimestampMillis();
  error = SaveFolderConfig(parent_path, *parent_config);
  if (error != VXCORE_OK) {
    return error;
  }

  const auto folder_relative_path = ConcatenatePaths(parent_path, folder_name);
  std::unique_ptr<FolderConfig> new_config(new FolderConfig(folder_name));
  error = SaveFolderConfig(folder_relative_path, *new_config);
  if (error != VXCORE_OK) {
    return error;
  }

  out_folder_id = new_config->id;
  CacheConfig(folder_relative_path, std::move(new_config));
  return VXCORE_OK;
}

VxCoreError FolderManager::DeleteFolder(const std::string &folder_path) {
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

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

VxCoreError FolderManager::CreateFile(const std::string &folder_path, const std::string &file_name,
                                      std::string &out_file_id) {
  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  std::string content_path = GetContentPath(folder_path);
  fs::path file_path = fs::path(content_path) / file_name;

  if (fs::exists(file_path)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  FolderConfig *config = nullptr;
  VxCoreError error = GetFolderConfig(folder_path, &config);
  if (error != VXCORE_OK) {
    return error;
  }

  if (FindFileRecord(*config, file_name)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  try {
    std::ofstream(file_path).close();
  } catch (const std::exception &) {
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
