#include "folder_manager.h"

#include <filesystem>
#include <fstream>

#include "utils/utils.h"

namespace vxcore {

namespace fs = std::filesystem;

FolderManager::FolderManager(Notebook *notebook) : notebook_(notebook) {}

FolderManager::~FolderManager() {}

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

void FolderManager::CacheConfig(const std::string &folder_path, const FolderConfig &config) {
  config_cache_[folder_path] = std::make_unique<FolderConfig>(config);
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

VxCoreError FolderManager::LoadFolderConfig(const std::string &folder_path, FolderConfig &config) {
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
    config = FolderConfig::FromJson(json);
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

    InvalidateCache(folder_path);
    return VXCORE_OK;
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }
}

VxCoreError FolderManager::GetFolderConfig(const std::string &folder_path,
                                           std::string &out_config_json) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig *cached = GetCachedConfig(folder_path);
  if (cached) {
    out_config_json = cached->ToJson().dump();
    return VXCORE_OK;
  }

  FolderConfig config;
  VxCoreError error = LoadFolderConfig(folder_path, config);
  if (error != VXCORE_OK) {
    return error;
  }

  CacheConfig(folder_path, config);
  out_config_json = config.ToJson().dump();
  return VXCORE_OK;
}

VxCoreError FolderManager::CreateFolder(const std::string &parent_path,
                                        const std::string &folder_name,
                                        std::string &out_folder_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  std::string content_path = GetContentPath(parent_path);
  fs::path folder_path = fs::path(content_path) / folder_name;

  try {
    if (fs::exists(folder_path)) {
      return VXCORE_ERR_ALREADY_EXISTS;
    }

    fs::create_directories(folder_path);

    FolderConfig parent_config;
    VxCoreError error = LoadFolderConfig(parent_path, parent_config);
    if (error != VXCORE_OK) {
      parent_config = FolderConfig("");
    }

    auto it = std::find(parent_config.folders.begin(), parent_config.folders.end(), folder_name);
    if (it == parent_config.folders.end()) {
      parent_config.folders.push_back(folder_name);
    }

    error = SaveFolderConfig(parent_path, parent_config);
    if (error != VXCORE_OK) {
      return error;
    }

    std::string new_folder_path =
        parent_path.empty() || parent_path == "." ? folder_name : parent_path + "/" + folder_name;
    FolderConfig new_config(folder_name);
    error = SaveFolderConfig(new_folder_path, new_config);
    if (error != VXCORE_OK) {
      return error;
    }

    out_folder_id = new_config.id;
    return VXCORE_OK;
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }
}

VxCoreError FolderManager::DeleteFolder(const std::string &folder_path) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  std::string content_path = GetContentPath(folder_path);

  try {
    if (!fs::exists(content_path)) {
      return VXCORE_ERR_NOT_FOUND;
    }

    fs::remove_all(content_path);

    size_t last_slash = folder_path.find_last_of("/\\");
    std::string parent_path;
    std::string folder_name;

    if (last_slash == std::string::npos) {
      parent_path = ".";
      folder_name = folder_path;
    } else {
      parent_path = folder_path.substr(0, last_slash);
      folder_name = folder_path.substr(last_slash + 1);
    }

    FolderConfig parent_config;
    VxCoreError error = LoadFolderConfig(parent_path, parent_config);
    if (error == VXCORE_OK) {
      auto it = std::find(parent_config.folders.begin(), parent_config.folders.end(), folder_name);
      if (it != parent_config.folders.end()) {
        parent_config.folders.erase(it);
        SaveFolderConfig(parent_path, parent_config);
      }
    }

    std::string config_path = GetConfigPath(folder_path);
    if (fs::exists(config_path)) {
      fs::remove_all(fs::path(config_path).parent_path());
    }

    InvalidateCache(folder_path);
    InvalidateCache(parent_path);

    return VXCORE_OK;
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }
}

VxCoreError FolderManager::UpdateFolderMetadata(const std::string &folder_path,
                                                const std::string &metadata_json) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig config;
  VxCoreError error = LoadFolderConfig(folder_path, config);
  if (error != VXCORE_OK) {
    return error;
  }

  try {
    nlohmann::json metadata = nlohmann::json::parse(metadata_json);
    if (!metadata.is_object()) {
      return VXCORE_ERR_JSON_PARSE;
    }

    config.metadata = metadata;
    config.modified_utc = GetCurrentTimestampMillis();

    return SaveFolderConfig(folder_path, config);
  } catch (const std::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  }
}

VxCoreError FolderManager::TrackFile(const std::string &folder_path, const std::string &file_name,
                                     std::string &out_file_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  std::string content_path = GetContentPath(folder_path);
  fs::path file_path = fs::path(content_path) / file_name;

  if (!fs::exists(file_path)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  FolderConfig config;
  VxCoreError error = LoadFolderConfig(folder_path, config);
  if (error != VXCORE_OK) {
    config = FolderConfig("");
  }

  FileRecord *existing = FindFileRecord(config, file_name);
  if (existing) {
    out_file_id = existing->id;
    return VXCORE_OK;
  }

  FileRecord new_file(file_name);

  try {
    auto file_time = fs::last_write_time(file_path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    auto timestamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch()).count();

    new_file.created_utc = timestamp;
    new_file.modified_utc = timestamp;
  } catch (const std::exception &) {
  }

  config.files.push_back(new_file);
  config.modified_utc = GetCurrentTimestampMillis();

  error = SaveFolderConfig(folder_path, config);
  if (error != VXCORE_OK) {
    return error;
  }

  out_file_id = new_file.id;
  return VXCORE_OK;
}

VxCoreError FolderManager::UntrackFile(const std::string &folder_path,
                                       const std::string &file_name) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig config;
  VxCoreError error = LoadFolderConfig(folder_path, config);
  if (error != VXCORE_OK) {
    return error;
  }

  auto it = std::find_if(config.files.begin(), config.files.end(),
                         [&file_name](const FileRecord &file) { return file.name == file_name; });

  if (it == config.files.end()) {
    return VXCORE_ERR_NOT_FOUND;
  }

  config.files.erase(it);
  config.modified_utc = GetCurrentTimestampMillis();

  return SaveFolderConfig(folder_path, config);
}

VxCoreError FolderManager::UpdateFileMetadata(const std::string &folder_path,
                                              const std::string &file_name,
                                              const std::string &metadata_json) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig config;
  VxCoreError error = LoadFolderConfig(folder_path, config);
  if (error != VXCORE_OK) {
    return error;
  }

  FileRecord *file = FindFileRecord(config, file_name);
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

    config.modified_utc = file->modified_utc;

    return SaveFolderConfig(folder_path, config);
  } catch (const std::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  }
}

VxCoreError FolderManager::UpdateFileTags(const std::string &folder_path,
                                          const std::string &file_name,
                                          const std::string &tags_json) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (notebook_->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  FolderConfig config;
  VxCoreError error = LoadFolderConfig(folder_path, config);
  if (error != VXCORE_OK) {
    return error;
  }

  FileRecord *file = FindFileRecord(config, file_name);
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

    config.modified_utc = file->modified_utc;

    return SaveFolderConfig(folder_path, config);
  } catch (const std::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  }
}

VxCoreError FolderManager::ListFolder(const std::string &folder_path,
                                      std::string &out_contents_json) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string content_path = GetContentPath(folder_path);

  if (!fs::exists(content_path)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  if (!fs::is_directory(content_path)) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  try {
    nlohmann::json result;
    result["tracked_files"] = nlohmann::json::array();
    result["tracked_folders"] = nlohmann::json::array();
    result["all_files"] = nlohmann::json::array();
    result["all_folders"] = nlohmann::json::array();

    if (notebook_->GetType() == NotebookType::Bundled) {
      FolderConfig config;
      VxCoreError error = LoadFolderConfig(folder_path, config);
      if (error == VXCORE_OK) {
        for (const auto &file : config.files) {
          result["tracked_files"].push_back(file.ToJson());
        }
        result["tracked_folders"] = config.folders;
      }
    }

    for (const auto &entry : fs::directory_iterator(content_path)) {
      std::string name = entry.path().filename().string();

      if (entry.is_directory()) {
        if (name != "vx_notebook") {
          result["all_folders"].push_back(name);
        }
      } else if (entry.is_regular_file()) {
        nlohmann::json file_info;
        file_info["name"] = name;

        auto file_time = fs::last_write_time(entry.path());
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        auto timestamp =
            std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch()).count();

        file_info["modified_utc"] = timestamp;

        result["all_files"].push_back(file_info);
      }
    }

    out_contents_json = result.dump();
    return VXCORE_OK;
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }
}

void FolderManager::ClearCache() {
  std::lock_guard<std::mutex> lock(mutex_);
  config_cache_.clear();
}

}  // namespace vxcore
