#include "raw_folder_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "metadata_store.h"
#include "notebook.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace fs = std::filesystem;

namespace vxcore {

RawFolderManager::RawFolderManager(Notebook *notebook) : FolderManager(notebook) {
  assert(notebook && notebook->GetType() == NotebookType::Raw);
}

RawFolderManager::~RawFolderManager() {}

VxCoreError RawFolderManager::InitRootFolder() {
  if (!root_folder_id_.empty()) {
    return VXCORE_OK;
  }

  auto *store = notebook_->GetMetadataStore();
  if (!store || !store->IsOpen()) {
    return VXCORE_ERR_INVALID_STATE;
  }

  // Check if root folder already exists in DB (parent_id is empty for root)
  auto root_folders = store->ListFolders("");
  for (const auto &f : root_folders) {
    if (f.name == ".") {
      root_folder_id_ = f.id;
      return VXCORE_OK;
    }
  }

  // Create synthetic root folder
  root_folder_id_ = GenerateUUID();
  auto now = GetCurrentTimestampMillis();
  StoreFolderRecord root_record;
  root_record.id = root_folder_id_;
  root_record.parent_id = "";  // NULL parent = root
  root_record.name = ".";
  root_record.created_utc = now;
  root_record.modified_utc = now;
  root_record.metadata = "{}";

  if (!store->CreateFolder(root_record)) {
    VXCORE_LOG_ERROR("InitRootFolder: Failed to create synthetic root folder");
    root_folder_id_.clear();
    return VXCORE_ERR_IO;
  }

  return VXCORE_OK;
}

VxCoreError RawFolderManager::EnsureFolderAncestorChain(const std::string &folder_path) {
  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_path = GetCleanRelativePath(folder_path);
  if (clean_path.empty() || clean_path == ".") {
    return VXCORE_OK;
  }

  auto *store = notebook_->GetMetadataStore();
  if (!store) {
    return VXCORE_ERR_INVALID_STATE;
  }

  // Build ancestor chain and create missing folders
  auto components = SplitPathComponents(clean_path);
  std::string current_parent_id = root_folder_id_;
  std::string current_path;

  for (const auto &component : components) {
    current_path = current_path.empty() ? component : ConcatenatePaths(current_path, component);

    // Look for existing folder by scanning parent's children
    bool found = false;
    auto children = store->ListFolders(current_parent_id);
    for (const auto &child : children) {
      if (child.name == component) {
        current_parent_id = child.id;
        found = true;
        break;
      }
    }
    if (found) {
      continue;
    }

    // Verify directory exists on filesystem
    auto abs_path = PathFromUtf8(ConcatenatePaths(notebook_->GetRootFolder(), current_path));
    if (!fs::is_directory(abs_path)) {
      return VXCORE_ERR_NOT_FOUND;
    }

    auto now = GetCurrentTimestampMillis();
    StoreFolderRecord record;
    record.id = GenerateUUID();
    record.parent_id = current_parent_id;
    record.name = component;
    record.created_utc = now;
    record.modified_utc = now;
    record.metadata = "{}";

    if (!store->CreateFolder(record)) {
      VXCORE_LOG_ERROR("EnsureFolderAncestorChain: Failed to create folder: %s",
                       current_path.c_str());
      return VXCORE_ERR_IO;
    }

    current_parent_id = record.id;
  }

  return VXCORE_OK;
}

VxCoreError RawFolderManager::SyncFolderFromFilesystem(const std::string &folder_path,
                                                       FolderContents &out_contents) {
  out_contents.files.clear();
  out_contents.folders.clear();

  auto *store = notebook_->GetMetadataStore();
  if (!store) {
    return VXCORE_ERR_INVALID_STATE;
  }

  const auto clean_path = GetCleanRelativePath(folder_path);
  const bool is_root = clean_path.empty() || clean_path == ".";

  // Get absolute filesystem path
  std::string abs_path_str =
      is_root ? notebook_->GetRootFolder()
              : ConcatenatePaths(notebook_->GetRootFolder(), clean_path);
  auto abs_fs_path = PathFromUtf8(abs_path_str);

  if (!fs::is_directory(abs_fs_path)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  // Get the folder's DB id
  std::string folder_db_id;
  if (is_root) {
    folder_db_id = root_folder_id_;
  } else {
    // Find folder by traversing from root
    auto components = SplitPathComponents(clean_path);
    std::string parent_id = root_folder_id_;
    for (const auto &comp : components) {
      bool found = false;
      auto children = store->ListFolders(parent_id);
      for (const auto &child : children) {
        if (child.name == comp) {
          parent_id = child.id;
          found = true;
          break;
        }
      }
      if (!found) {
        return VXCORE_ERR_NOT_FOUND;
      }
    }
    folder_db_id = parent_id;
  }

  // Get existing DB records for this folder
  auto db_files = store->ListFiles(folder_db_id);
  auto db_folders = store->ListFolders(folder_db_id);

  // Build lookup maps
  std::map<std::string, StoreFileRecord> db_file_map;
  for (auto &f : db_files) {
    db_file_map[f.name] = std::move(f);
  }
  std::map<std::string, StoreFolderRecord> db_folder_map;
  for (auto &f : db_folders) {
    db_folder_map[f.name] = std::move(f);
  }

  // Scan filesystem
  std::set<std::string> fs_files;
  std::set<std::string> fs_folders;

  try {
    for (const auto &entry : fs::directory_iterator(abs_fs_path)) {
      std::string entry_name = PathToUtf8(entry.path().filename());

      // Skip hidden entries
      if (entry_name.empty() || entry_name[0] == '.') {
        continue;
      }

      if (entry.is_directory()) {
        fs_folders.insert(entry_name);
      } else if (entry.is_regular_file()) {
        fs_files.insert(entry_name);
      }
    }
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("SyncFolderFromFilesystem: Failed to iterate: %s", e.what());
    return VXCORE_ERR_IO;
  }

  // Begin transaction for batch operations
  store->BeginTransaction();

  // Remove stale DB file records (files no longer on filesystem)
  for (const auto &[name, record] : db_file_map) {
    if (fs_files.find(name) == fs_files.end()) {
      store->DeleteFile(record.id);
    }
  }

  // Remove stale DB folder records
  for (const auto &[name, record] : db_folder_map) {
    if (fs_folders.find(name) == fs_folders.end()) {
      store->DeleteFolder(record.id);
    }
  }

  // Sync files: create new DB records for new filesystem files, keep existing
  for (const auto &file_name : fs_files) {
    auto it = db_file_map.find(file_name);
    if (it != db_file_map.end()) {
      // Existing file — convert to FileRecord
      const auto &sf = it->second;
      FileRecord fr;
      fr.id = sf.id;
      fr.name = sf.name;
      fr.created_utc = sf.created_utc;
      fr.modified_utc = sf.modified_utc;
      if (!sf.metadata.empty() && sf.metadata != "{}") {
        try {
          fr.metadata = nlohmann::json::parse(sf.metadata);
        } catch (...) {
          fr.metadata = nlohmann::json::object();
        }
      } else {
        fr.metadata = nlohmann::json::object();
      }
      fr.tags = sf.tags;
      fr.attachments = sf.attachments;
      out_contents.files.push_back(std::move(fr));
    } else {
      // New file — create DB record
      auto now = GetCurrentTimestampMillis();
      StoreFileRecord new_record;
      new_record.id = GenerateUUID();
      new_record.folder_id = folder_db_id;
      new_record.name = file_name;
      new_record.created_utc = now;
      new_record.modified_utc = now;
      new_record.metadata = "{}";

      store->CreateFile(new_record);

      FileRecord fr;
      fr.id = new_record.id;
      fr.name = file_name;
      fr.created_utc = now;
      fr.modified_utc = now;
      fr.metadata = nlohmann::json::object();
      out_contents.files.push_back(std::move(fr));
    }
  }

  // Sync folders: ensure DB records exist for filesystem folders
  for (const auto &folder_name : fs_folders) {
    auto it = db_folder_map.find(folder_name);
    if (it == db_folder_map.end()) {
      // New folder — create DB record
      auto now = GetCurrentTimestampMillis();
      StoreFolderRecord new_record;
      new_record.id = GenerateUUID();
      new_record.parent_id = folder_db_id;
      new_record.name = folder_name;
      new_record.created_utc = now;
      new_record.modified_utc = now;
      new_record.metadata = "{}";

      store->CreateFolder(new_record);

      // Store in map for later use
      db_folder_map[folder_name] = new_record;
    }
  }

  store->CommitTransaction();

  return VXCORE_OK;
}

namespace {

std::string FindFolderIdByPath(MetadataStore *store, const std::string &root_folder_id,
                               const std::string &path) {
  if (path.empty() || path == ".") {
    return root_folder_id;
  }
  auto components = SplitPathComponents(path);
  std::string parent_id = root_folder_id;
  for (const auto &comp : components) {
    bool found = false;
    auto children = store->ListFolders(parent_id);
    for (const auto &child : children) {
      if (child.name == comp) {
        parent_id = child.id;
        found = true;
        break;
      }
    }
    if (!found) {
      return "";
    }
  }
  return parent_id;
}

std::string FindFileIdByPath(MetadataStore *store, const std::string &root_folder_id,
                             const std::string &file_path) {
  auto [folder_path, file_name] = SplitPath(file_path);
  std::string folder_id = FindFolderIdByPath(store, root_folder_id, folder_path);
  if (folder_id.empty()) return "";
  auto files = store->ListFiles(folder_id);
  for (const auto &f : files) {
    if (f.name == file_name) return f.id;
  }
  return "";
}

}  // namespace

VxCoreError RawFolderManager::GetFolderConfig(const std::string &folder_path,
                                              std::string &out_config_json) {
  out_config_json.clear();

  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_path = GetCleanRelativePath(folder_path);

  // Ensure ancestor chain exists
  err = EnsureFolderAncestorChain(clean_path);
  if (err != VXCORE_OK) {
    return err;
  }

  // List contents to sync with filesystem
  FolderContents contents;
  err = SyncFolderFromFilesystem(clean_path, contents);
  if (err != VXCORE_OK) {
    return err;
  }

  auto *store = notebook_->GetMetadataStore();
  const bool is_root = clean_path.empty() || clean_path == ".";

  // Get folder record from DB
  FolderConfig config;
  if (is_root) {
    auto root_rec = store->GetFolder(root_folder_id_);
    if (root_rec.has_value()) {
      config.id = root_rec->id;
      config.name = root_rec->name;
      config.created_utc = root_rec->created_utc;
      config.modified_utc = root_rec->modified_utc;
    }
  } else {
    std::string folder_id = FindFolderIdByPath(store, root_folder_id_, clean_path);
    if (folder_id.empty()) {
      return VXCORE_ERR_NOT_FOUND;
    }
    auto folder_rec = store->GetFolder(folder_id);
    if (!folder_rec.has_value()) {
      return VXCORE_ERR_NOT_FOUND;
    }
    config.id = folder_rec->id;
    config.name = folder_rec->name;
    config.created_utc = folder_rec->created_utc;
    config.modified_utc = folder_rec->modified_utc;
    if (!folder_rec->metadata.empty() && folder_rec->metadata != "{}") {
      try {
        config.metadata = nlohmann::json::parse(folder_rec->metadata);
      } catch (...) {
        config.metadata = nlohmann::json::object();
      }
    }
  }

  config.files = contents.files;

  // Re-read subfolder names from DB after sync
  std::string folder_id_for_children =
      is_root ? root_folder_id_ : FindFolderIdByPath(store, root_folder_id_, clean_path);
  auto db_subfolders = store->ListFolders(folder_id_for_children);
  for (const auto &sf : db_subfolders) {
    config.folders.push_back(sf.name);
  }

  out_config_json = config.ToJson().dump();
  return VXCORE_OK;
}

VxCoreError RawFolderManager::CreateFolder(const std::string &parent_path,
                                           const std::string &folder_name,
                                           std::string &out_folder_id) {
  VXCORE_LOG_INFO("RawFolderManager::CreateFolder: parent=%s, name=%s", parent_path.c_str(),
                  folder_name.c_str());

  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_parent = GetCleanRelativePath(parent_path);

  // Build absolute path for the new folder
  std::string parent_abs =
      (clean_parent.empty() || clean_parent == ".")
          ? notebook_->GetRootFolder()
          : ConcatenatePaths(notebook_->GetRootFolder(), clean_parent);
  std::string new_abs = ConcatenatePaths(parent_abs, folder_name);
  auto new_fs_path = PathFromUtf8(new_abs);

  if (fs::exists(new_fs_path)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  // Filesystem first
  try {
    fs::create_directories(new_fs_path);
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  // Ensure parent chain exists in DB
  err = EnsureFolderAncestorChain(clean_parent);
  if (err != VXCORE_OK) {
    return err;
  }

  auto *store = notebook_->GetMetadataStore();
  if (!store) {
    return VXCORE_ERR_INVALID_STATE;
  }

  std::string parent_id = FindFolderIdByPath(store, root_folder_id_, clean_parent);
  if (parent_id.empty()) {
    return VXCORE_ERR_NOT_FOUND;
  }

  auto now = GetCurrentTimestampMillis();
  StoreFolderRecord record;
  record.id = GenerateUUID();
  record.parent_id = parent_id;
  record.name = folder_name;
  record.created_utc = now;
  record.modified_utc = now;
  record.metadata = "{}";

  if (!store->CreateFolder(record)) {
    VXCORE_LOG_WARN("Failed to write folder to MetadataStore: id=%s", record.id.c_str());
  }

  out_folder_id = record.id;
  return VXCORE_OK;
}

VxCoreError RawFolderManager::DeleteFolder(const std::string &folder_path) {
  VXCORE_LOG_INFO("RawFolderManager::DeleteFolder: path=%s", folder_path.c_str());

  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_path = GetCleanRelativePath(folder_path);
  if (clean_path.empty() || clean_path == ".") {
    return VXCORE_ERR_INVALID_PARAM;
  }

  std::string abs_path = ConcatenatePaths(notebook_->GetRootFolder(), clean_path);
  auto fs_path = PathFromUtf8(abs_path);

  if (!fs::exists(fs_path)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  // Get folder ID before deleting
  auto *store = notebook_->GetMetadataStore();
  std::string folder_id;
  if (store) {
    folder_id = FindFolderIdByPath(store, root_folder_id_, clean_path);
  }

  // Filesystem first — permanent delete
  try {
    fs::remove_all(fs_path);
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  // DB second
  if (store && !folder_id.empty()) {
    if (!store->DeleteFolder(folder_id)) {
      VXCORE_LOG_WARN("Failed to delete folder from MetadataStore: id=%s", folder_id.c_str());
    }
  }

  return VXCORE_OK;
}

VxCoreError RawFolderManager::UpdateFolderMetadata(const std::string &folder_path,
                                                   const std::string &metadata_json) {
  (void)folder_path;
  (void)metadata_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::UpdateNodeTimestamps(const std::string &node_path,
                                                   int64_t created_utc, int64_t modified_utc) {
  (void)node_path;
  (void)created_utc;
  (void)modified_utc;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::GetFolderMetadata(const std::string &folder_path,
                                                std::string &out_metadata_json) {
  (void)folder_path;
  (void)out_metadata_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::RenameFolder(const std::string &folder_path,
                                           const std::string &new_name) {
  VXCORE_LOG_INFO("RawFolderManager::RenameFolder: path=%s, new_name=%s", folder_path.c_str(),
                  new_name.c_str());

  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_path = GetCleanRelativePath(folder_path);
  if (clean_path.empty() || clean_path == ".") {
    return VXCORE_ERR_INVALID_PARAM;
  }

  const auto [parent_path, old_name] = SplitPath(clean_path);
  std::string old_abs = ConcatenatePaths(notebook_->GetRootFolder(), clean_path);
  auto old_fs = PathFromUtf8(old_abs);

  if (!fs::exists(old_fs)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  std::string new_rel = ConcatenatePaths(parent_path, new_name);
  std::string new_abs = ConcatenatePaths(notebook_->GetRootFolder(), new_rel);
  auto new_fs = PathFromUtf8(new_abs);

  if (fs::exists(new_fs)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  // Filesystem first
  try {
    fs::rename(old_fs, new_fs);
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  // DB second — update name
  auto *store = notebook_->GetMetadataStore();
  if (store) {
    std::string folder_id = FindFolderIdByPath(store, root_folder_id_, clean_path);
    if (!folder_id.empty()) {
      auto now = GetCurrentTimestampMillis();
      if (!store->UpdateFolder(folder_id, new_name, now, "{}")) {
        VXCORE_LOG_WARN("Failed to rename folder in MetadataStore: id=%s", folder_id.c_str());
      }
    }
  }

  return VXCORE_OK;
}

VxCoreError RawFolderManager::MoveFolder(const std::string &src_path,
                                         const std::string &dest_parent_path) {
  VXCORE_LOG_INFO("RawFolderManager::MoveFolder: src=%s, dest=%s", src_path.c_str(),
                  dest_parent_path.c_str());

  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_src = GetCleanRelativePath(src_path);
  const auto clean_dest = GetCleanRelativePath(dest_parent_path);

  if (clean_src.empty() || clean_src == ".") {
    return VXCORE_ERR_INVALID_PARAM;
  }

  const auto [src_parent, folder_name] = SplitPath(clean_src);
  std::string src_abs = ConcatenatePaths(notebook_->GetRootFolder(), clean_src);
  auto src_fs = PathFromUtf8(src_abs);

  if (!fs::exists(src_fs)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  std::string dest_folder_rel = ConcatenatePaths(clean_dest, folder_name);
  std::string dest_abs = ConcatenatePaths(notebook_->GetRootFolder(), dest_folder_rel);
  auto dest_fs = PathFromUtf8(dest_abs);

  if (fs::exists(dest_fs)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  // Ensure dest parent exists on filesystem
  std::string dest_parent_abs =
      (clean_dest.empty() || clean_dest == ".")
          ? notebook_->GetRootFolder()
          : ConcatenatePaths(notebook_->GetRootFolder(), clean_dest);
  if (!fs::is_directory(PathFromUtf8(dest_parent_abs))) {
    return VXCORE_ERR_NOT_FOUND;
  }

  // Filesystem first
  try {
    fs::rename(src_fs, dest_fs);
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  // DB second — move folder to new parent
  auto *store = notebook_->GetMetadataStore();
  if (store) {
    std::string folder_id = FindFolderIdByPath(store, root_folder_id_, clean_src);
    if (!folder_id.empty()) {
      // Ensure dest parent chain in DB
      err = EnsureFolderAncestorChain(clean_dest);
      if (err == VXCORE_OK) {
        std::string dest_parent_id = FindFolderIdByPath(store, root_folder_id_, clean_dest);
        if (!dest_parent_id.empty()) {
          if (!store->MoveFolder(folder_id, dest_parent_id)) {
            VXCORE_LOG_WARN("Failed to move folder in MetadataStore: id=%s", folder_id.c_str());
          }
        }
      }
    }
  }

  return VXCORE_OK;
}

VxCoreError RawFolderManager::CopyFolder(const std::string &src_path,
                                         const std::string &dest_parent_path,
                                         const std::string &new_name,
                                         std::string &out_folder_id) {
  VXCORE_LOG_INFO("RawFolderManager::CopyFolder: src=%s, dest=%s, name=%s", src_path.c_str(),
                  dest_parent_path.c_str(), new_name.c_str());

  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_src = GetCleanRelativePath(src_path);
  const auto clean_dest = GetCleanRelativePath(dest_parent_path);

  if (clean_src.empty() || clean_src == ".") {
    return VXCORE_ERR_INVALID_PARAM;
  }

  const auto [src_parent, src_folder_name] = SplitPath(clean_src);
  const std::string folder_name = new_name.empty() ? src_folder_name : new_name;

  std::string src_abs = ConcatenatePaths(notebook_->GetRootFolder(), clean_src);
  auto src_fs = PathFromUtf8(src_abs);

  if (!fs::exists(src_fs)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  std::string dest_rel = ConcatenatePaths(clean_dest, folder_name);
  std::string dest_abs = ConcatenatePaths(notebook_->GetRootFolder(), dest_rel);
  auto dest_fs = PathFromUtf8(dest_abs);

  if (fs::exists(dest_fs)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  // Filesystem first — recursive copy
  try {
    fs::copy(src_fs, dest_fs, fs::copy_options::recursive);
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  // DB second — create new folder record with fresh UUID
  auto *store = notebook_->GetMetadataStore();
  out_folder_id = GenerateUUID();

  if (store) {
    // Ensure dest parent chain in DB
    err = EnsureFolderAncestorChain(clean_dest);
    if (err == VXCORE_OK) {
      std::string dest_parent_id = FindFolderIdByPath(store, root_folder_id_, clean_dest);
      if (!dest_parent_id.empty()) {
        auto now = GetCurrentTimestampMillis();
        StoreFolderRecord record;
        record.id = out_folder_id;
        record.parent_id = dest_parent_id;
        record.name = folder_name;
        record.created_utc = now;
        record.modified_utc = now;
        record.metadata = "{}";

        if (!store->CreateFolder(record)) {
          VXCORE_LOG_WARN("Failed to create copied folder in MetadataStore: id=%s",
                          out_folder_id.c_str());
        }
      }
    }
  }

  return VXCORE_OK;
}

VxCoreError RawFolderManager::CreateFile(const std::string &folder_path,
                                         const std::string &file_name,
                                         std::string &out_file_id) {
  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_folder = GetCleanRelativePath(folder_path);
  const bool is_root = clean_folder.empty() || clean_folder == ".";
  const std::string folder_abs =
      is_root ? notebook_->GetRootFolder()
              : ConcatenatePaths(notebook_->GetRootFolder(), clean_folder);
  const std::string file_abs = ConcatenatePaths(folder_abs, file_name);
  auto fs_file = PathFromUtf8(file_abs);

  if (fs::exists(fs_file)) {
    VXCORE_LOG_WARN("File already exists: %s", file_abs.c_str());
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  // Filesystem first
  try {
    std::ofstream(fs_file).close();
    VXCORE_LOG_DEBUG("Created file: %s", file_abs.c_str());
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to create file: %s", e.what());
    return VXCORE_ERR_IO;
  }

  // DB second
  auto *store = notebook_->GetMetadataStore();
  if (store) {
    err = EnsureFolderAncestorChain(clean_folder);
    if (err != VXCORE_OK) {
      VXCORE_LOG_WARN("Failed to ensure ancestor chain for: %s", clean_folder.c_str());
    } else {
      std::string folder_id = FindFolderIdByPath(store, root_folder_id_, clean_folder);
      if (!folder_id.empty()) {
        auto now = GetCurrentTimestampMillis();
        out_file_id = GenerateUUID();
        StoreFileRecord record;
        record.id = out_file_id;
        record.folder_id = folder_id;
        record.name = file_name;
        record.created_utc = now;
        record.modified_utc = now;
        record.metadata = "{}";
        if (!store->CreateFile(record)) {
          VXCORE_LOG_WARN("Failed to create file in MetadataStore: id=%s", out_file_id.c_str());
        }
      }
    }
  }

  if (out_file_id.empty()) {
    out_file_id = GenerateUUID();
  }

  return VXCORE_OK;
}

VxCoreError RawFolderManager::DeleteFile(const std::string &file_path) {
  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_path = GetCleanRelativePath(file_path);
  const auto [folder_path, file_name] = SplitPath(clean_path);
  VXCORE_LOG_INFO("DeleteFile: file_path=%s", clean_path.c_str());

  const bool is_root = folder_path.empty() || folder_path == ".";
  const std::string folder_abs =
      is_root ? notebook_->GetRootFolder()
              : ConcatenatePaths(notebook_->GetRootFolder(), folder_path);
  const std::string file_abs = ConcatenatePaths(folder_abs, file_name);
  auto fs_file = PathFromUtf8(file_abs);

  if (!fs::exists(fs_file)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  // Filesystem first — permanent delete
  try {
    fs::remove(fs_file);
    VXCORE_LOG_DEBUG("Deleted file: %s", file_abs.c_str());
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to delete file: %s", e.what());
    return VXCORE_ERR_IO;
  }

  // DB second
  auto *store = notebook_->GetMetadataStore();
  if (store) {
    std::string file_id = FindFileIdByPath(store, root_folder_id_, clean_path);
    if (!file_id.empty()) {
      if (!store->DeleteFile(file_id)) {
        VXCORE_LOG_WARN("Failed to delete file from MetadataStore: id=%s", file_id.c_str());
      }
    }
  }

  VXCORE_LOG_INFO("DeleteFile successful: %s", clean_path.c_str());
  return VXCORE_OK;
}

VxCoreError RawFolderManager::UpdateFileMetadata(const std::string &file_path,
                                                 const std::string &metadata_json) {
  (void)file_path;
  (void)metadata_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::UpdateFileTags(const std::string &file_path,
                                             const std::string &tags_json) {
  (void)file_path;
  (void)tags_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::TagFile(const std::string &file_path, const std::string &tag_name) {
  (void)file_path;
  (void)tag_name;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::UntagFile(const std::string &file_path,
                                        const std::string &tag_name) {
  (void)file_path;
  (void)tag_name;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::GetFileInfo(const std::string &file_path,
                                          std::string &out_file_info_json) {
  const FileRecord *record = nullptr;
  VxCoreError err = GetFileInfo(file_path, &record);
  if (err != VXCORE_OK) {
    return err;
  }
  out_file_info_json = record->ToJson().dump();
  return VXCORE_OK;
}

VxCoreError RawFolderManager::GetFileInfo(const std::string &file_path,
                                          const FileRecord **out_record) {
  *out_record = nullptr;

  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_path = GetCleanRelativePath(file_path);
  const auto [folder_path, file_name] = SplitPath(clean_path);

  // Ensure ancestor chain for the parent folder
  err = EnsureFolderAncestorChain(folder_path);
  if (err != VXCORE_OK) {
    return err;
  }

  // Sync the parent folder to discover the file
  FolderContents contents;
  err = SyncFolderFromFilesystem(folder_path, contents);
  if (err != VXCORE_OK) {
    return err;
  }

  // Find the file in the synced contents
  for (const auto &f : contents.files) {
    if (f.name == file_name) {
      last_queried_file_ = f;
      *out_record = &last_queried_file_;
      return VXCORE_OK;
    }
  }

  return VXCORE_ERR_NOT_FOUND;
}

VxCoreError RawFolderManager::GetFileMetadata(const std::string &file_path,
                                              std::string &out_metadata_json) {
  (void)file_path;
  (void)out_metadata_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::RenameFile(const std::string &file_path,
                                         const std::string &new_name) {
  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_path = GetCleanRelativePath(file_path);
  const auto [folder_path, old_name] = SplitPath(clean_path);
  VXCORE_LOG_INFO("RenameFile: file_path=%s, new_name=%s", clean_path.c_str(), new_name.c_str());

  const bool is_root = folder_path.empty() || folder_path == ".";
  const std::string folder_abs =
      is_root ? notebook_->GetRootFolder()
              : ConcatenatePaths(notebook_->GetRootFolder(), folder_path);
  const std::string old_abs = ConcatenatePaths(folder_abs, old_name);
  const std::string new_abs = ConcatenatePaths(folder_abs, new_name);
  auto fs_old = PathFromUtf8(old_abs);
  auto fs_new = PathFromUtf8(new_abs);

  if (!fs::exists(fs_old)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  if (fs::exists(fs_new)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  // Filesystem first
  try {
    fs::rename(fs_old, fs_new);
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  // DB second — UUID preserved
  auto *store = notebook_->GetMetadataStore();
  if (store) {
    std::string file_id = FindFileIdByPath(store, root_folder_id_, clean_path);
    if (!file_id.empty()) {
      auto now = GetCurrentTimestampMillis();
      if (!store->UpdateFile(file_id, new_name, now, "{}")) {
        VXCORE_LOG_WARN("Failed to update file in MetadataStore: id=%s", file_id.c_str());
      }
    }
  }

  VXCORE_LOG_INFO("RenameFile successful: %s -> %s", clean_path.c_str(),
                  ConcatenatePaths(folder_path, new_name).c_str());
  return VXCORE_OK;
}

VxCoreError RawFolderManager::MoveFile(const std::string &file_path,
                                       const std::string &dest_folder_path) {
  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_src = GetCleanRelativePath(file_path);
  const auto clean_dest = GetCleanRelativePath(dest_folder_path);
  const auto [src_folder, file_name] = SplitPath(clean_src);
  VXCORE_LOG_INFO("MoveFile: src=%s, dest=%s", clean_src.c_str(), clean_dest.c_str());

  const bool src_is_root = src_folder.empty() || src_folder == ".";
  const std::string src_folder_abs =
      src_is_root ? notebook_->GetRootFolder()
                  : ConcatenatePaths(notebook_->GetRootFolder(), src_folder);
  const std::string src_abs = ConcatenatePaths(src_folder_abs, file_name);

  const bool dest_is_root = clean_dest.empty() || clean_dest == ".";
  const std::string dest_folder_abs =
      dest_is_root ? notebook_->GetRootFolder()
                   : ConcatenatePaths(notebook_->GetRootFolder(), clean_dest);
  const std::string dest_abs = ConcatenatePaths(dest_folder_abs, file_name);

  auto fs_src = PathFromUtf8(src_abs);
  auto fs_dest = PathFromUtf8(dest_abs);

  if (!fs::exists(fs_src)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  if (fs::exists(fs_dest)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  // Filesystem first
  try {
    fs::rename(fs_src, fs_dest);
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  // DB second — UUID preserved, move to new parent
  auto *store = notebook_->GetMetadataStore();
  if (store) {
    std::string file_id = FindFileIdByPath(store, root_folder_id_, clean_src);
    if (!file_id.empty()) {
      err = EnsureFolderAncestorChain(clean_dest);
      if (err == VXCORE_OK) {
        std::string dest_folder_id = FindFolderIdByPath(store, root_folder_id_, clean_dest);
        if (!dest_folder_id.empty()) {
          if (!store->MoveFile(file_id, dest_folder_id)) {
            VXCORE_LOG_WARN("Failed to move file in MetadataStore: id=%s", file_id.c_str());
          }
        }
      }
    }
  }

  VXCORE_LOG_INFO("MoveFile successful: %s -> %s/%s", clean_src.c_str(), clean_dest.c_str(),
                  file_name.c_str());
  return VXCORE_OK;
}

VxCoreError RawFolderManager::CopyFile(const std::string &file_path,
                                       const std::string &dest_folder_path,
                                       const std::string &new_name, std::string &out_file_id) {
  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_src = GetCleanRelativePath(file_path);
  const auto clean_dest = GetCleanRelativePath(dest_folder_path);
  const auto [src_folder, file_name] = SplitPath(clean_src);
  const std::string target_name = new_name.empty() ? file_name : new_name;
  VXCORE_LOG_INFO("CopyFile: src=%s, dest=%s, target=%s", clean_src.c_str(), clean_dest.c_str(),
                  target_name.c_str());

  const bool src_is_root = src_folder.empty() || src_folder == ".";
  const std::string src_folder_abs =
      src_is_root ? notebook_->GetRootFolder()
                  : ConcatenatePaths(notebook_->GetRootFolder(), src_folder);
  const std::string src_abs = ConcatenatePaths(src_folder_abs, file_name);

  const bool dest_is_root = clean_dest.empty() || clean_dest == ".";
  const std::string dest_folder_abs =
      dest_is_root ? notebook_->GetRootFolder()
                   : ConcatenatePaths(notebook_->GetRootFolder(), clean_dest);
  const std::string dest_abs = ConcatenatePaths(dest_folder_abs, target_name);

  auto fs_src = PathFromUtf8(src_abs);
  auto fs_dest = PathFromUtf8(dest_abs);

  if (!fs::exists(fs_src)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  if (fs::exists(fs_dest)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  // Filesystem first
  try {
    fs::copy_file(fs_src, fs_dest);
  } catch (const std::exception &) {
    return VXCORE_ERR_IO;
  }

  // DB second — FRESH UUID
  out_file_id = GenerateUUID();
  auto *store = notebook_->GetMetadataStore();
  if (store) {
    err = EnsureFolderAncestorChain(clean_dest);
    if (err == VXCORE_OK) {
      std::string dest_folder_id = FindFolderIdByPath(store, root_folder_id_, clean_dest);
      if (!dest_folder_id.empty()) {
        auto now = GetCurrentTimestampMillis();
        StoreFileRecord record;
        record.id = out_file_id;
        record.folder_id = dest_folder_id;
        record.name = target_name;
        record.created_utc = now;
        record.modified_utc = now;
        record.metadata = "{}";
        if (!store->CreateFile(record)) {
          VXCORE_LOG_WARN("Failed to create copied file in MetadataStore: id=%s",
                          out_file_id.c_str());
        }
      }
    }
  }

  VXCORE_LOG_INFO("CopyFile successful: id=%s", out_file_id.c_str());
  return VXCORE_OK;
}

VxCoreError RawFolderManager::ImportFile(const std::string &folder_path,
                                         const std::string &external_file_path,
                                         std::string &out_file_id) {
  VXCORE_LOG_INFO("ImportFile: folder=%s, external_file=%s", folder_path.c_str(),
                  external_file_path.c_str());

  const fs::path external_fs = PathFromUtf8(external_file_path);

  // Validate external file exists and is a regular file
  if (!fs::exists(external_fs)) {
    VXCORE_LOG_ERROR("ImportFile: External file not found: %s", external_file_path.c_str());
    return VXCORE_ERR_NOT_FOUND;
  }
  if (!fs::is_regular_file(external_fs)) {
    VXCORE_LOG_ERROR("ImportFile: Path is not a regular file: %s", external_file_path.c_str());
    return VXCORE_ERR_INVALID_PARAM;
  }

  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_folder = GetCleanRelativePath(folder_path);
  const bool is_root = clean_folder.empty() || clean_folder == ".";

  // Resolve destination folder to absolute path
  const std::string dest_folder_abs =
      is_root ? notebook_->GetRootFolder()
              : ConcatenatePaths(notebook_->GetRootFolder(), clean_folder);

  // Ensure destination folder exists on filesystem
  auto dest_folder_fs = PathFromUtf8(dest_folder_abs);
  if (!fs::is_directory(dest_folder_fs)) {
    VXCORE_LOG_ERROR("ImportFile: Destination folder not found: %s", dest_folder_abs.c_str());
    return VXCORE_ERR_NOT_FOUND;
  }

  // Extract filename and handle name collision
  std::string original_name = PathToUtf8(external_fs.filename());
  std::string target_name;
  err = GetAvailableName(clean_folder, original_name, target_name);
  if (err != VXCORE_OK) {
    target_name = original_name;  // Fallback
  }

  fs::path dest_path = dest_folder_fs / PathFromUtf8(target_name);

  // Filesystem first — copy the file
  try {
    fs::copy_file(external_fs, dest_path);
    VXCORE_LOG_DEBUG("ImportFile: Copied file to: %s", PathToUtf8(dest_path).c_str());
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("ImportFile: Failed to copy file: %s", e.what());
    return VXCORE_ERR_IO;
  }

  // DB second — create record with fresh UUID
  out_file_id = GenerateUUID();
  auto *store = notebook_->GetMetadataStore();
  if (store) {
    err = EnsureFolderAncestorChain(clean_folder);
    if (err == VXCORE_OK) {
      std::string dest_folder_id = FindFolderIdByPath(store, root_folder_id_, clean_folder);
      if (!dest_folder_id.empty()) {
        auto now = GetCurrentTimestampMillis();
        StoreFileRecord record;
        record.id = out_file_id;
        record.folder_id = dest_folder_id;
        record.name = target_name;
        record.created_utc = now;
        record.modified_utc = now;
        record.metadata = "{}";
        if (!store->CreateFile(record)) {
          VXCORE_LOG_WARN("ImportFile: Failed to create file in MetadataStore: id=%s",
                          out_file_id.c_str());
        }
      }
    }
  }

  VXCORE_LOG_INFO("ImportFile: Successfully imported file: id=%s, name=%s", out_file_id.c_str(),
                  target_name.c_str());
  return VXCORE_OK;
}

VxCoreError RawFolderManager::ImportFolder(const std::string &dest_folder_path,
                                           const std::string &external_folder_path,
                                           const std::string &suffix_allowlist,
                                           std::string &out_folder_id) {
  VXCORE_LOG_INFO("ImportFolder: dest=%s, external=%s, suffix_allowlist=%s",
                  dest_folder_path.c_str(), external_folder_path.c_str(),
                  suffix_allowlist.c_str());

  fs::path external_path = PathFromUtf8(external_folder_path);

  // Validate external folder exists and is a directory
  if (!fs::exists(external_path)) {
    VXCORE_LOG_ERROR("ImportFolder: External folder not found: %s",
                     external_folder_path.c_str());
    return VXCORE_ERR_NOT_FOUND;
  }
  if (!fs::is_directory(external_path)) {
    VXCORE_LOG_ERROR("ImportFolder: Path is not a directory: %s",
                     external_folder_path.c_str());
    return VXCORE_ERR_INVALID_PARAM;
  }

  // Reject importing from within the notebook root
  try {
    fs::path canonical_external = fs::canonical(external_path);
    fs::path root_path = PathFromUtf8(notebook_->GetRootFolder());
    auto mismatch_pair = std::mismatch(root_path.begin(), root_path.end(),
                                       canonical_external.begin(), canonical_external.end());
    if (mismatch_pair.first == root_path.end()) {
      VXCORE_LOG_ERROR("ImportFolder: Cannot import folder from within notebook root: %s",
                       external_folder_path.c_str());
      return VXCORE_ERR_INVALID_PARAM;
    }
  } catch (const std::exception &e) {
    VXCORE_LOG_WARN("ImportFolder: Failed to canonicalize paths: %s", e.what());
  }

  // Parse suffix allowlist
  std::set<std::string> allowed_suffixes;
  if (!suffix_allowlist.empty()) {
    std::istringstream iss(suffix_allowlist);
    std::string suffix;
    while (std::getline(iss, suffix, ';')) {
      if (!suffix.empty()) {
        if (suffix[0] == '.') {
          suffix = suffix.substr(1);
        }
        std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        allowed_suffixes.insert(suffix);
      }
    }
    VXCORE_LOG_DEBUG("ImportFolder: Parsed %zu allowed suffixes", allowed_suffixes.size());
  }

  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_dest = GetCleanRelativePath(dest_folder_path);
  const bool dest_is_root = clean_dest.empty() || clean_dest == ".";
  const std::string dest_abs =
      dest_is_root ? notebook_->GetRootFolder()
                   : ConcatenatePaths(notebook_->GetRootFolder(), clean_dest);

  // Determine unique target name
  std::string original_name = PathToUtf8(external_path.filename());
  std::string target_name = original_name;
  fs::path target_path = PathFromUtf8(dest_abs) / PathFromUtf8(target_name);
  if (fs::exists(target_path)) {
    int name_suffix = 1;
    while (name_suffix <= 10000) {
      target_name = original_name + "_" + std::to_string(name_suffix);
      target_path = PathFromUtf8(dest_abs) / PathFromUtf8(target_name);
      if (!fs::exists(target_path)) {
        break;
      }
      ++name_suffix;
    }
    if (name_suffix > 10000) {
      target_name = original_name + "_" + std::to_string(GetCurrentTimestampMillis());
      target_path = PathFromUtf8(dest_abs) / PathFromUtf8(target_name);
    }
  }

  // Recursive copy with suffix filtering (filesystem first)
  std::function<void(const fs::path &, const fs::path &)> copy_filtered =
      [&](const fs::path &src, const fs::path &dest) {
        fs::create_directories(dest);
        for (const auto &entry : fs::directory_iterator(src)) {
          const std::string entry_name = PathToUtf8(entry.path().filename());
          // Skip hidden files/folders
          if (entry_name.empty() || entry_name[0] == '.') {
            continue;
          }
          if (entry.is_directory()) {
            copy_filtered(entry.path(), dest / PathFromUtf8(entry_name));
          } else if (entry.is_regular_file()) {
            if (!allowed_suffixes.empty()) {
              std::string ext = PathToUtf8(entry.path().extension());
              if (!ext.empty() && ext[0] == '.') {
                ext = ext.substr(1);
              }
              std::transform(ext.begin(), ext.end(), ext.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
              if (allowed_suffixes.find(ext) == allowed_suffixes.end()) {
                continue;
              }
            }
            fs::copy_file(entry.path(), dest / PathFromUtf8(entry_name),
                          fs::copy_options::overwrite_existing);
          }
        }
      };

  try {
    copy_filtered(external_path, target_path);
    VXCORE_LOG_DEBUG("ImportFolder: Copied folder to: %s", PathToUtf8(target_path).c_str());
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("ImportFolder: Failed to copy folder: %s", e.what());
    return VXCORE_ERR_IO;
  }

  // DB second — create records for imported folder and contents
  out_folder_id = GenerateUUID();
  auto *store = notebook_->GetMetadataStore();
  if (store) {
    err = EnsureFolderAncestorChain(clean_dest);
    if (err != VXCORE_OK) {
      VXCORE_LOG_WARN("ImportFolder: Failed to ensure ancestor chain for dest: %s",
                      clean_dest.c_str());
    }

    std::string dest_folder_id = FindFolderIdByPath(store, root_folder_id_, clean_dest);
    if (dest_folder_id.empty()) {
      VXCORE_LOG_WARN("ImportFolder: Could not find dest folder in DB");
    }

    store->BeginTransaction();

    // Create root imported folder record
    auto now = GetCurrentTimestampMillis();
    StoreFolderRecord root_record;
    root_record.id = out_folder_id;
    root_record.parent_id = dest_folder_id;
    root_record.name = target_name;
    root_record.created_utc = now;
    root_record.modified_utc = now;
    root_record.metadata = "{}";
    if (!store->CreateFolder(root_record)) {
      VXCORE_LOG_WARN("ImportFolder: Failed to create root folder in DB: id=%s",
                      out_folder_id.c_str());
    }

    // Recursively create DB records for all contents
    std::function<void(const fs::path &, const std::string &)> index_db =
        [&](const fs::path &dir, const std::string &parent_id) {
          if (!fs::is_directory(dir)) return;
          for (const auto &entry : fs::directory_iterator(dir)) {
            const std::string entry_name = PathToUtf8(entry.path().filename());
            if (entry_name.empty() || entry_name[0] == '.') {
              continue;
            }
            if (entry.is_directory()) {
              std::string sub_id = GenerateUUID();
              auto ts = GetCurrentTimestampMillis();
              StoreFolderRecord sub_record;
              sub_record.id = sub_id;
              sub_record.parent_id = parent_id;
              sub_record.name = entry_name;
              sub_record.created_utc = ts;
              sub_record.modified_utc = ts;
              sub_record.metadata = "{}";
              if (!store->CreateFolder(sub_record)) {
                VXCORE_LOG_WARN("ImportFolder: Failed to create subfolder in DB: %s",
                                entry_name.c_str());
              }
              index_db(entry.path(), sub_id);
            } else if (entry.is_regular_file()) {
              std::string file_id = GenerateUUID();
              auto ts = GetCurrentTimestampMillis();
              StoreFileRecord file_record;
              file_record.id = file_id;
              file_record.folder_id = parent_id;
              file_record.name = entry_name;
              file_record.created_utc = ts;
              file_record.modified_utc = ts;
              file_record.metadata = "{}";
              if (!store->CreateFile(file_record)) {
                VXCORE_LOG_WARN("ImportFolder: Failed to create file in DB: %s",
                                entry_name.c_str());
              }
            }
          }
        };

    index_db(target_path, out_folder_id);

    store->CommitTransaction();
  }

  VXCORE_LOG_INFO("ImportFolder: Successfully imported folder: id=%s, name=%s",
                  out_folder_id.c_str(), target_name.c_str());
  return VXCORE_OK;
}

void RawFolderManager::IterateAllFiles(
    std::function<bool(const std::string &, const FileRecord &)> callback) {
  (void)callback;
}

VxCoreError RawFolderManager::FindFilesByTag(const std::string &tag_name,
                                             std::string &out_files_json) {
  (void)tag_name;
  (void)out_files_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::ListFolderContents(const std::string &folder_path,
                                                 bool include_folders_info,
                                                 FolderContents &out_contents) {
  VxCoreError err = InitRootFolder();
  if (err != VXCORE_OK) {
    return err;
  }

  const auto clean_path = GetCleanRelativePath(folder_path);

  // Ensure ancestor chain for this folder
  err = EnsureFolderAncestorChain(clean_path);
  if (err != VXCORE_OK) {
    return err;
  }

  // Sync filesystem with DB
  FolderContents synced;
  err = SyncFolderFromFilesystem(clean_path, synced);
  if (err != VXCORE_OK) {
    return err;
  }

  out_contents.files = std::move(synced.files);
  out_contents.folders.clear();

  auto *store = notebook_->GetMetadataStore();
  std::string folder_db_id = FindFolderIdByPath(store, root_folder_id_, clean_path);
  if (folder_db_id.empty()) {
    return VXCORE_ERR_NOT_FOUND;
  }

  auto db_folders = store->ListFolders(folder_db_id);

  // Filter to only folders that exist on filesystem (SyncFolderFromFilesystem already cleaned stale)
  for (const auto &sf : db_folders) {
    if (!include_folders_info) {
      // Name-only placeholder (critical for SearchManager traversal)
      out_contents.folders.emplace_back(sf.name);
    } else {
      nlohmann::json meta = nlohmann::json::object();
      if (!sf.metadata.empty() && sf.metadata != "{}") {
        try {
          meta = nlohmann::json::parse(sf.metadata);
        } catch (...) {
        }
      }
      out_contents.folders.emplace_back(sf.id, sf.name, sf.created_utc, sf.modified_utc, meta);
    }
  }

  return VXCORE_OK;
}

VxCoreError RawFolderManager::ListExternalNodes(const std::string &folder_path,
                                                FolderContents &out_contents) {
  (void)folder_path;
  (void)out_contents;
  // Raw notebooks don't track external nodes - all nodes are external by definition
  return VXCORE_OK;
}

void RawFolderManager::ClearCache() { root_folder_id_.clear(); }

VxCoreError RawFolderManager::IndexNode(const std::string &node_path) {
  (void)node_path;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::UnindexNode(const std::string &node_path) {
  (void)node_path;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::GetFileAttachments(const std::string &file_path,
                                                 std::string &out_attachments_json) {
  (void)file_path;
  (void)out_attachments_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::UpdateFileAttachments(const std::string &file_path,
                                                    const std::string &attachments_json) {
  (void)file_path;
  (void)attachments_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::AddFileAttachment(const std::string &file_path,
                                                const std::string &attachment) {
  (void)file_path;
  (void)attachment;
  return VXCORE_ERR_UNSUPPORTED;
}

VxCoreError RawFolderManager::DeleteFileAttachment(const std::string &file_path,
                                                   const std::string &attachment) {
  (void)file_path;
  (void)attachment;
  return VXCORE_ERR_UNSUPPORTED;
}

}  // namespace vxcore
