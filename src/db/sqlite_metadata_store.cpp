#include "sqlite_metadata_store.h"

#include <sqlite3.h>

#include "db_manager.h"
#include "file_db.h"
#include "notebook_db.h"
#include "tag_db.h"
#include "utils/file_utils.h"
#include "utils/logger.h"

namespace vxcore {
namespace db {

SqliteMetadataStore::SqliteMetadataStore()
    : db_manager_(std::make_unique<DbManager>()),
      file_db_(nullptr),
      tag_db_(nullptr),
      notebook_db_(nullptr) {}

SqliteMetadataStore::~SqliteMetadataStore() { Close(); }

// --- Lifecycle ---

bool SqliteMetadataStore::Open(const std::string& db_path) {
  if (IsOpen()) {
    Close();
  }

  if (!db_manager_->Open(db_path)) {
    last_error_ = "Failed to open database: " + db_manager_->GetLastError();
    VXCORE_LOG_ERROR("%s", last_error_.c_str());
    return false;
  }

  if (!db_manager_->InitializeSchema()) {
    last_error_ = "Failed to initialize schema: " + db_manager_->GetLastError();
    VXCORE_LOG_ERROR("%s", last_error_.c_str());
    db_manager_->Close();
    return false;
  }

  // Create FileDb and TagDb with the open database handle
  file_db_ = std::make_unique<FileDb>(db_manager_->GetHandle());
  tag_db_ = std::make_unique<TagDb>(db_manager_->GetHandle());
  notebook_db_ = std::make_unique<NotebookDb>(db_manager_->GetHandle());

  VXCORE_LOG_INFO("SqliteMetadataStore opened: %s", db_path.c_str());
  return true;
}

void SqliteMetadataStore::Close() {
  file_db_.reset();
  tag_db_.reset();
  notebook_db_.reset();
  db_manager_->Close();
  VXCORE_LOG_DEBUG("SqliteMetadataStore closed");
}

bool SqliteMetadataStore::IsOpen() const { return db_manager_->IsOpen(); }

// --- Transaction Management ---

bool SqliteMetadataStore::BeginTransaction() {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }
  return db_manager_->BeginTransaction();
}

bool SqliteMetadataStore::CommitTransaction() {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }
  return db_manager_->CommitTransaction();
}

bool SqliteMetadataStore::RollbackTransaction() {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }
  return db_manager_->RollbackTransaction();
}

// --- Internal Helpers ---

int64_t SqliteMetadataStore::GetFolderDbId(const std::string& folder_uuid) {
  if (folder_uuid.empty()) {
    return -1;  // Root folder
  }
  auto folder = file_db_->GetFolderByUuid(folder_uuid);
  return folder ? folder->id : -1;
}

int64_t SqliteMetadataStore::GetFileDbId(const std::string& file_uuid) {
  auto file = file_db_->GetFileByUuid(file_uuid);
  return file ? file->id : -1;
}

std::string SqliteMetadataStore::GetFolderUuid(int64_t folder_db_id) {
  if (folder_db_id == -1) {
    return "";  // Root folder
  }
  auto folder = file_db_->GetFolder(folder_db_id);
  return folder ? folder->uuid : "";
}

std::string SqliteMetadataStore::GetFileUuid(int64_t file_db_id) {
  auto file = file_db_->GetFile(file_db_id);
  return file ? file->uuid : "";
}

StoreFolderRecord SqliteMetadataStore::ToStoreFolderRecord(const DbFolderRecord& db_record) {
  StoreFolderRecord record;
  record.id = db_record.uuid;
  record.parent_id = GetFolderUuid(db_record.parent_id);
  record.name = db_record.name;
  record.created_utc = db_record.created_utc;
  record.modified_utc = db_record.modified_utc;
  record.metadata = db_record.metadata;
  return record;
}

StoreFileRecord SqliteMetadataStore::ToStoreFileRecord(const DbFileRecord& db_record) {
  StoreFileRecord record;
  record.id = db_record.uuid;
  record.folder_id = GetFolderUuid(db_record.folder_id);
  record.name = db_record.name;
  record.created_utc = db_record.created_utc;
  record.modified_utc = db_record.modified_utc;
  record.metadata = db_record.metadata;
  record.tags = db_record.tags;
  return record;
}

// --- Folder Operations ---

bool SqliteMetadataStore::CreateFolder(const StoreFolderRecord& folder) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  int64_t parent_db_id = GetFolderDbId(folder.parent_id);

  int64_t folder_id =
      file_db_->CreateOrUpdateFolder(folder.id, parent_db_id, folder.name, folder.created_utc,
                                     folder.modified_utc, folder.metadata);
  if (folder_id == -1) {
    last_error_ = "Failed to create folder: " + file_db_->GetLastError();
    return false;
  }

  return true;
}

bool SqliteMetadataStore::UpdateFolder(const std::string& folder_id, const std::string& name,
                                       int64_t modified_utc, const std::string& metadata) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  int64_t db_id = GetFolderDbId(folder_id);
  if (db_id == -1) {
    last_error_ = "Folder not found: " + folder_id;
    return false;
  }

  // FileDb::UpdateFolder doesn't update metadata, so we need to use CreateOrUpdateFolder
  auto existing = file_db_->GetFolder(db_id);
  if (!existing) {
    last_error_ = "Folder not found: " + folder_id;
    return false;
  }

  int64_t result = file_db_->CreateOrUpdateFolder(existing->uuid, existing->parent_id, name,
                                                  existing->created_utc, modified_utc, metadata);
  if (result == -1) {
    last_error_ = "Failed to update folder: " + file_db_->GetLastError();
    return false;
  }

  return true;
}

bool SqliteMetadataStore::DeleteFolder(const std::string& folder_id) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  int64_t db_id = GetFolderDbId(folder_id);
  if (db_id == -1) {
    last_error_ = "Folder not found: " + folder_id;
    return false;
  }

  if (!file_db_->DeleteFolder(db_id)) {
    last_error_ = "Failed to delete folder: " + file_db_->GetLastError();
    return false;
  }

  return true;
}

std::optional<StoreFolderRecord> SqliteMetadataStore::GetFolder(const std::string& folder_id) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return std::nullopt;
  }

  auto db_folder = file_db_->GetFolderByUuid(folder_id);
  if (!db_folder) {
    return std::nullopt;
  }

  return ToStoreFolderRecord(*db_folder);
}

std::optional<StoreFolderRecord> SqliteMetadataStore::GetFolderByPath(const std::string& path) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return std::nullopt;
  }

  auto db_folder = file_db_->GetFolderByPath(path);
  if (!db_folder) {
    return std::nullopt;
  }

  return ToStoreFolderRecord(*db_folder);
}

std::vector<StoreFolderRecord> SqliteMetadataStore::ListFolders(const std::string& parent_id) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return {};
  }

  int64_t parent_db_id = GetFolderDbId(parent_id);
  auto db_folders = file_db_->ListFolders(parent_db_id);

  std::vector<StoreFolderRecord> result;
  result.reserve(db_folders.size());
  for (const auto& db_folder : db_folders) {
    result.push_back(ToStoreFolderRecord(db_folder));
  }
  return result;
}

std::string SqliteMetadataStore::GetFolderPath(const std::string& folder_id) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return "";
  }

  int64_t db_id = GetFolderDbId(folder_id);
  if (db_id == -1) {
    return "";
  }

  return file_db_->GetFolderPath(db_id);
}

std::string SqliteMetadataStore::GetNodePathById(const std::string& node_id) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return "";
  }

  // Helper to strip leading './' from path
  auto strip_root_prefix = [](const std::string& path) -> std::string {
    if (path.size() >= 2 && path[0] == '.' && path[1] == '/') {
      return path.substr(2);
    }
    if (path == ".") {
      return "";  // Root folder itself
    }
    return path;
  };

  // Try folder first
  auto folder = file_db_->GetFolderByUuid(node_id);
  if (folder) {
    return strip_root_prefix(file_db_->GetFolderPath(folder->id));
  }

  // Try file
  auto file = file_db_->GetFileByUuid(node_id);
  if (file) {
    std::string folder_path = strip_root_prefix(file_db_->GetFolderPath(file->folder_id));
    if (folder_path.empty()) {
      return file->name;  // File in root
    }
    return folder_path + "/" + file->name;
  }

  return "";  // Not found
}

bool SqliteMetadataStore::MoveFolder(const std::string& folder_id,
                                     const std::string& new_parent_id) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  int64_t db_id = GetFolderDbId(folder_id);
  if (db_id == -1) {
    last_error_ = "Folder not found: " + folder_id;
    return false;
  }

  int64_t new_parent_db_id = GetFolderDbId(new_parent_id);

  if (!file_db_->MoveFolder(db_id, new_parent_db_id)) {
    last_error_ = "Failed to move folder: " + file_db_->GetLastError();
    return false;
  }

  return true;
}

// --- File Operations ---

bool SqliteMetadataStore::CreateFile(const StoreFileRecord& file) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  int64_t folder_db_id = GetFolderDbId(file.folder_id);
  if (folder_db_id == -1 && !file.folder_id.empty()) {
    last_error_ = "Parent folder not found: " + file.folder_id;
    return false;
  }

  int64_t file_id = file_db_->CreateOrUpdateFile(file.id, folder_db_id, file.name, file.created_utc,
                                                 file.modified_utc, file.metadata);
  if (file_id == -1) {
    last_error_ = "Failed to create file: " + file_db_->GetLastError();
    return false;
  }

  // Set tags if any
  if (!file.tags.empty()) {
    if (!file_db_->SetFileTags(file_id, file.tags)) {
      last_error_ = "Failed to set file tags: " + file_db_->GetLastError();
      return false;
    }
  }

  return true;
}

bool SqliteMetadataStore::UpdateFile(const std::string& file_id, const std::string& name,
                                     int64_t modified_utc, const std::string& metadata) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  int64_t db_id = GetFileDbId(file_id);
  if (db_id == -1) {
    last_error_ = "File not found: " + file_id;
    return false;
  }

  // Get existing file to preserve folder_id and created_utc
  auto existing = file_db_->GetFile(db_id);
  if (!existing) {
    last_error_ = "File not found: " + file_id;
    return false;
  }

  int64_t result = file_db_->CreateOrUpdateFile(existing->uuid, existing->folder_id, name,
                                                existing->created_utc, modified_utc, metadata);
  if (result == -1) {
    last_error_ = "Failed to update file: " + file_db_->GetLastError();
    return false;
  }

  return true;
}

bool SqliteMetadataStore::DeleteFile(const std::string& file_id) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  int64_t db_id = GetFileDbId(file_id);
  if (db_id == -1) {
    last_error_ = "File not found: " + file_id;
    return false;
  }

  if (!file_db_->DeleteFile(db_id)) {
    last_error_ = "Failed to delete file: " + file_db_->GetLastError();
    return false;
  }

  return true;
}

std::optional<StoreFileRecord> SqliteMetadataStore::GetFile(const std::string& file_id) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return std::nullopt;
  }

  auto db_file = file_db_->GetFileByUuid(file_id);
  if (!db_file) {
    return std::nullopt;
  }

  return ToStoreFileRecord(*db_file);
}

std::optional<StoreFileRecord> SqliteMetadataStore::GetFileByPath(const std::string& path) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return std::nullopt;
  }

  // Split path into folder path and file name
  std::string clean_path = CleanPath(path);
  auto [folder_path, file_name] = SplitPath(clean_path);

  // Get folder (empty folder_path means root)
  int64_t folder_db_id = -1;
  if (!folder_path.empty()) {
    auto folder = file_db_->GetFolderByPath(folder_path);
    if (!folder) {
      return std::nullopt;  // Folder not found
    }
    folder_db_id = folder->id;
  }

  // Get file by name in folder
  auto db_file = file_db_->GetFileByName(folder_db_id, file_name);
  if (!db_file) {
    return std::nullopt;
  }

  return ToStoreFileRecord(*db_file);
}

std::vector<StoreFileRecord> SqliteMetadataStore::ListFiles(const std::string& folder_id) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return {};
  }

  int64_t folder_db_id = GetFolderDbId(folder_id);
  auto db_files = file_db_->ListFiles(folder_db_id);

  std::vector<StoreFileRecord> result;
  result.reserve(db_files.size());
  for (const auto& db_file : db_files) {
    result.push_back(ToStoreFileRecord(db_file));
  }
  return result;
}

bool SqliteMetadataStore::MoveFile(const std::string& file_id, const std::string& new_folder_id) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  int64_t db_id = GetFileDbId(file_id);
  if (db_id == -1) {
    last_error_ = "File not found: " + file_id;
    return false;
  }

  int64_t new_folder_db_id = GetFolderDbId(new_folder_id);

  if (!file_db_->MoveFile(db_id, new_folder_db_id)) {
    last_error_ = "Failed to move file: " + file_db_->GetLastError();
    return false;
  }

  return true;
}

// --- Tag Definition Operations ---

bool SqliteMetadataStore::CreateOrUpdateTag(const StoreTagRecord& tag) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  // Get parent_id from parent_name
  int64_t parent_id = -1;
  if (!tag.parent_name.empty()) {
    auto parent_tag = tag_db_->GetTag(tag.parent_name);
    if (parent_tag) {
      parent_id = parent_tag->id;
    }
  }

  int64_t tag_id = tag_db_->CreateOrUpdateTag(tag.name, parent_id, tag.metadata);
  if (tag_id == -1) {
    last_error_ = "Failed to create/update tag: " + tag_db_->GetLastError();
    return false;
  }

  return true;
}

bool SqliteMetadataStore::DeleteTag(const std::string& tag_name) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  auto tag = tag_db_->GetTag(tag_name);
  if (!tag) {
    last_error_ = "Tag not found: " + tag_name;
    return false;
  }

  if (!tag_db_->DeleteTag(tag->id)) {
    last_error_ = "Failed to delete tag: " + tag_db_->GetLastError();
    return false;
  }

  return true;
}

std::optional<StoreTagRecord> SqliteMetadataStore::GetTag(const std::string& tag_name) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return std::nullopt;
  }

  auto db_tag = tag_db_->GetTag(tag_name);
  if (!db_tag) {
    return std::nullopt;
  }

  StoreTagRecord record;
  record.name = db_tag->name;
  record.metadata = db_tag->metadata;

  // Get parent name if has parent
  if (db_tag->parent_id != -1) {
    auto parent = tag_db_->GetTagById(db_tag->parent_id);
    if (parent) {
      record.parent_name = parent->name;
    }
  }

  return record;
}

std::vector<StoreTagRecord> SqliteMetadataStore::ListTags() {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return {};
  }

  auto db_tags = tag_db_->ListAllTags();

  // Build a map of id -> name for parent lookup
  std::unordered_map<int64_t, std::string> id_to_name;
  for (const auto& db_tag : db_tags) {
    id_to_name[db_tag.id] = db_tag.name;
  }

  std::vector<StoreTagRecord> result;
  result.reserve(db_tags.size());
  for (const auto& db_tag : db_tags) {
    StoreTagRecord record;
    record.name = db_tag.name;
    record.metadata = db_tag.metadata;
    if (db_tag.parent_id != -1) {
      auto it = id_to_name.find(db_tag.parent_id);
      if (it != id_to_name.end()) {
        record.parent_name = it->second;
      }
    }
    result.push_back(record);
  }
  return result;
}

// --- File-Tag Association Operations ---

bool SqliteMetadataStore::SetFileTags(const std::string& file_id,
                                      const std::vector<std::string>& tags) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  int64_t db_id = GetFileDbId(file_id);
  if (db_id == -1) {
    last_error_ = "File not found: " + file_id;
    return false;
  }

  if (!file_db_->SetFileTags(db_id, tags)) {
    last_error_ = "Failed to set file tags: " + file_db_->GetLastError();
    return false;
  }

  return true;
}

bool SqliteMetadataStore::AddTagToFile(const std::string& file_id, const std::string& tag_name) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  int64_t db_id = GetFileDbId(file_id);
  if (db_id == -1) {
    last_error_ = "File not found: " + file_id;
    return false;
  }

  if (!file_db_->AddTagToFile(db_id, tag_name)) {
    last_error_ = "Failed to add tag to file: " + file_db_->GetLastError();
    return false;
  }

  return true;
}

bool SqliteMetadataStore::RemoveTagFromFile(const std::string& file_id,
                                            const std::string& tag_name) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  int64_t db_id = GetFileDbId(file_id);
  if (db_id == -1) {
    last_error_ = "File not found: " + file_id;
    return false;
  }

  // Get current tags
  auto current_tags = file_db_->GetFileTags(db_id);

  // Remove the specified tag
  std::vector<std::string> new_tags;
  for (const auto& tag : current_tags) {
    if (tag != tag_name) {
      new_tags.push_back(tag);
    }
  }

  // Set updated tags
  if (!file_db_->SetFileTags(db_id, new_tags)) {
    last_error_ = "Failed to remove tag from file: " + file_db_->GetLastError();
    return false;
  }

  return true;
}

std::vector<std::string> SqliteMetadataStore::GetFileTags(const std::string& file_id) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return {};
  }

  int64_t db_id = GetFileDbId(file_id);
  if (db_id == -1) {
    return {};
  }

  return file_db_->GetFileTags(db_id);
}

// --- Tag Query Operations ---

std::vector<StoreTagQueryResult> SqliteMetadataStore::FindFilesByTagsOr(
    const std::vector<std::string>& tags) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return {};
  }

  auto db_results = tag_db_->FindFilesByTagsOr(tags);

  std::vector<StoreTagQueryResult> results;
  results.reserve(db_results.size());
  for (const auto& db_result : db_results) {
    StoreTagQueryResult result;
    result.file_id = GetFileUuid(db_result.file_id);
    result.folder_id = GetFolderUuid(db_result.folder_id);
    result.file_name = db_result.file_name;
    result.tags = db_result.tags;

    // Compute file_path
    std::string folder_path = file_db_->GetFolderPath(db_result.folder_id);
    if (folder_path.empty()) {
      result.file_path = result.file_name;
    } else {
      result.file_path = folder_path + "/" + result.file_name;
    }

    results.push_back(result);
  }
  return results;
}

std::vector<StoreTagQueryResult> SqliteMetadataStore::FindFilesByTagsAnd(
    const std::vector<std::string>& tags) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return {};
  }

  auto db_results = tag_db_->FindFilesByTagsAnd(tags);

  std::vector<StoreTagQueryResult> results;
  results.reserve(db_results.size());
  for (const auto& db_result : db_results) {
    StoreTagQueryResult result;
    result.file_id = GetFileUuid(db_result.file_id);
    result.folder_id = GetFolderUuid(db_result.folder_id);
    result.file_name = db_result.file_name;
    result.tags = db_result.tags;

    // Compute file_path
    std::string folder_path = file_db_->GetFolderPath(db_result.folder_id);
    if (folder_path.empty()) {
      result.file_path = result.file_name;
    } else {
      result.file_path = folder_path + "/" + result.file_name;
    }

    results.push_back(result);
  }
  return results;
}

std::vector<std::pair<std::string, int>> SqliteMetadataStore::CountFilesByTag() {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return {};
  }

  return tag_db_->CountFilesByTag();
}

// --- Sync/Recovery Operations ---

std::optional<StoreSyncState> SqliteMetadataStore::GetSyncState(const std::string& folder_id) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return std::nullopt;
  }

  auto db_folder = file_db_->GetFolderByUuid(folder_id);
  if (!db_folder) {
    return std::nullopt;
  }

  if (db_folder->last_sync_utc == -1) {
    return std::nullopt;  // Never synced
  }

  StoreSyncState state;
  state.folder_id = folder_id;
  state.last_sync_utc = db_folder->last_sync_utc;
  state.config_file_modified_utc = db_folder->metadata_file_modified_utc;
  return state;
}

bool SqliteMetadataStore::UpdateSyncState(const std::string& folder_id, int64_t sync_time,
                                          int64_t config_file_modified_utc) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  int64_t db_id = GetFolderDbId(folder_id);
  if (db_id == -1) {
    last_error_ = "Folder not found: " + folder_id;
    return false;
  }

  // Update sync state using raw SQL (FileDb doesn't expose this)
  const char* sql =
      "UPDATE folders SET last_sync_utc = ?, metadata_file_modified_utc = ? WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;
  sqlite3* db = db_manager_->GetHandle();
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    last_error_ = "Failed to prepare sync state update: " + std::string(sqlite3_errmsg(db));
    return false;
  }

  sqlite3_bind_int64(stmt, 1, sync_time);
  sqlite3_bind_int64(stmt, 2, config_file_modified_utc);
  sqlite3_bind_int64(stmt, 3, db_id);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    last_error_ = "Failed to update sync state: " + std::string(sqlite3_errmsg(db));
    return false;
  }

  return true;
}

bool SqliteMetadataStore::ClearSyncState(const std::string& folder_id) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  int64_t db_id = GetFolderDbId(folder_id);
  if (db_id == -1) {
    last_error_ = "Folder not found: " + folder_id;
    return false;
  }

  // Clear sync state using raw SQL
  const char* sql =
      "UPDATE folders SET last_sync_utc = NULL, metadata_file_modified_utc = NULL WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;
  sqlite3* db = db_manager_->GetHandle();
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    last_error_ = "Failed to prepare sync state clear: " + std::string(sqlite3_errmsg(db));
    return false;
  }

  sqlite3_bind_int64(stmt, 1, db_id);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    last_error_ = "Failed to clear sync state: " + std::string(sqlite3_errmsg(db));
    return false;
  }

  return true;
}

bool SqliteMetadataStore::RebuildAll() {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return false;
  }

  if (!db_manager_->RebuildDatabase()) {
    last_error_ = "Failed to rebuild database: " + db_manager_->GetLastError();
    return false;
  }

  VXCORE_LOG_INFO("MetadataStore rebuilt (all data cleared)");
  return true;
}

// --- Iteration ---

void SqliteMetadataStore::IterateAllFiles(
    std::function<bool(const std::string&, const StoreFileRecord&)> callback) {
  if (!IsOpen()) {
    last_error_ = "Store not open";
    return;
  }

  // Get all files by iterating all folders
  std::function<void(int64_t)> iterate_folder = [&](int64_t folder_db_id) {
    // Get folder path
    std::string folder_path = file_db_->GetFolderPath(folder_db_id);

    // Iterate files in this folder
    auto files = file_db_->ListFiles(folder_db_id);
    for (const auto& db_file : files) {
      StoreFileRecord record = ToStoreFileRecord(db_file);
      std::string file_path = folder_path.empty() ? record.name : folder_path + "/" + record.name;

      if (!callback(file_path, record)) {
        return;  // Stop iteration
      }
    }

    // Recurse into subfolders
    auto subfolders = file_db_->ListFolders(folder_db_id);
    for (const auto& subfolder : subfolders) {
      iterate_folder(subfolder.id);
    }
  };

  // Start from root (-1)
  iterate_folder(-1);
}

std::optional<std::string> SqliteMetadataStore::GetNotebookMetadata(const std::string& key) {
  return notebook_db_->GetMetadata(key);
}

bool SqliteMetadataStore::SetNotebookMetadata(const std::string& key, const std::string& value) {
  return notebook_db_->SetMetadata(key, value);
}

// --- Error Handling ---

std::string SqliteMetadataStore::GetLastError() const { return last_error_; }

}  // namespace db
}  // namespace vxcore
