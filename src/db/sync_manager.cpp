#include "sync_manager.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>

#include "core/folder.h"
#include "file_db.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {
namespace db {

DbSyncManager::DbSyncManager(sqlite3* db, FileDb* file_db)
    : db_(db), file_db_(file_db), last_error_() {}

bool DbSyncManager::NeedsSynchronization(const std::string& folder_path,
                                         const std::string& filesystem_root) {
  // Try to find folder in DB by path
  auto folder = file_db_->GetFolderByPath(folder_path);
  if (!folder.has_value()) {
    return true;  // Folder not in DB → needs sync
  }

  // Check if ever synced
  if (folder->last_sync_utc == -1) {
    return true;  // Never synced before
  }

  // Compare JSON modification time
  int64_t json_modified = GetJsonModifiedTime(filesystem_root, folder_path);
  if (json_modified <= 0) {
    return false;  // No vx.json file
  }

  return json_modified > folder->metadata_file_modified_utc;
}

SyncResult DbSyncManager::SynchronizeFolder(const std::string& folder_path,
                                            const std::string& filesystem_root) {
  if (!db_ || !file_db_) {
    last_error_ = "Database or FileDb not initialized";
    return SyncResult::kDatabaseError;
  }

  VXCORE_LOG_DEBUG("Synchronizing folder: path=%s, root=%s", folder_path.c_str(),
                   filesystem_root.c_str());

  // Get JSON modification time
  int64_t json_modified = GetJsonModifiedTime(filesystem_root, folder_path);
  if (json_modified <= 0) {
    last_error_ = "vx.json file not found or inaccessible";
    return SyncResult::kJsonNotFound;
  }

  // Check if sync needed by looking up folder in DB
  auto folder = file_db_->GetFolderByPath(folder_path);
  if (folder.has_value() && folder->last_sync_utc != -1 &&
      json_modified <= folder->metadata_file_modified_utc) {
    VXCORE_LOG_DEBUG("No changes detected for folder: %s", folder_path.c_str());
    return SyncResult::kNoChanges;
  }

  // Determine parent_id for the folder
  int64_t parent_id = -1;
  if (!folder_path.empty() && folder_path != ".") {
    // Find parent folder path
    std::filesystem::path p(folder_path);
    if (p.has_parent_path() && p.parent_path() != p) {
      auto parent = file_db_->GetFolderByPath(p.parent_path().string());
      if (parent.has_value()) {
        parent_id = parent->id;
      }
    }
  }

  // Load and sync folder
  int64_t sync_time = GetCurrentTimestampMillis();
  if (!LoadFolderFromJson(filesystem_root, folder_path, parent_id)) {
    return SyncResult::kDatabaseError;
  }

  // Get the folder again after loading (it should exist now)
  folder = file_db_->GetFolderByPath(folder_path);
  if (!folder.has_value()) {
    last_error_ = "Failed to find folder after loading";
    return SyncResult::kDatabaseError;
  }

  // Update sync state
  if (!UpdateSyncState(folder->id, sync_time, json_modified)) {
    VXCORE_LOG_WARN("Failed to update sync state for: %s", folder_path.c_str());
  }

  VXCORE_LOG_INFO("Successfully synchronized folder: path=%s", folder_path.c_str());
  return SyncResult::kSuccess;
}

SyncResult DbSyncManager::RebuildFolder(const std::string& folder_path,
                                        const std::string& filesystem_root) {
  if (!db_ || !file_db_) {
    last_error_ = "Database or FileDb not initialized";
    return SyncResult::kDatabaseError;
  }

  VXCORE_LOG_INFO("Rebuilding folder: path=%s", folder_path.c_str());

  // Get JSON modification time
  int64_t json_modified = GetJsonModifiedTime(filesystem_root, folder_path);
  if (json_modified <= 0) {
    last_error_ = "vx.json file not found or inaccessible";
    return SyncResult::kJsonNotFound;
  }

  // Delete existing folder data from DB (lookup by path)
  auto folder = file_db_->GetFolderByPath(folder_path);
  int64_t parent_id = -1;
  if (folder.has_value()) {
    parent_id = folder->parent_id;  // Remember parent for re-creation
    if (!file_db_->DeleteFolder(folder->id)) {
      last_error_ = "Failed to delete existing folder data";
      return SyncResult::kDatabaseError;
    }
  } else {
    // Folder doesn't exist, find parent from path
    if (!folder_path.empty() && folder_path != ".") {
      std::filesystem::path p(folder_path);
      if (p.has_parent_path() && p.parent_path() != p) {
        auto parent = file_db_->GetFolderByPath(p.parent_path().string());
        if (parent.has_value()) {
          parent_id = parent->id;
        }
      }
    }
  }

  // Re-import from JSON
  int64_t sync_time = GetCurrentTimestampMillis();
  if (!LoadFolderFromJson(filesystem_root, folder_path, parent_id)) {
    return SyncResult::kDatabaseError;
  }

  // Get the folder again after loading
  folder = file_db_->GetFolderByPath(folder_path);
  if (!folder.has_value()) {
    last_error_ = "Failed to find folder after rebuilding";
    return SyncResult::kDatabaseError;
  }

  // Update sync state
  if (!UpdateSyncState(folder->id, sync_time, json_modified)) {
    VXCORE_LOG_WARN("Failed to update sync state for: %s", folder_path.c_str());
  }

  VXCORE_LOG_INFO("Successfully rebuilt folder: path=%s", folder_path.c_str());
  return SyncResult::kSuccess;
}

std::optional<FolderSyncState> DbSyncManager::GetSyncState(const std::string& folder_path) {
  auto folder = file_db_->GetFolderByPath(folder_path);
  if (!folder.has_value()) {
    return std::nullopt;  // Folder not found
  }

  // Check if ever synced
  if (folder->last_sync_utc == -1 && folder->metadata_file_modified_utc == -1) {
    return std::nullopt;  // Never synced
  }

  FolderSyncState state;
  state.folder_id = folder->id;
  state.last_sync_utc = folder->last_sync_utc;
  state.metadata_file_modified_utc = folder->metadata_file_modified_utc;
  return state;
}

bool DbSyncManager::ClearSyncState(const std::string& folder_path) {
  auto folder = file_db_->GetFolderByPath(folder_path);
  if (!folder.has_value()) {
    return false;  // Folder not found
  }

  const char* sql =
      "UPDATE folders SET last_sync_utc = NULL, metadata_file_modified_utc = NULL WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    VXCORE_LOG_ERROR("Failed to prepare clear sync state statement: %s", sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_int64(stmt, 1, folder->id);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

std::string DbSyncManager::GetLastError() const { return last_error_; }

// Private helper methods

int64_t DbSyncManager::GetJsonModifiedTime(const std::string& filesystem_root,
                                           const std::string& folder_path) {
  try {
    std::filesystem::path vx_json_path =
        std::filesystem::path(filesystem_root) / "vx_notebook" / "notes";
    if (!folder_path.empty() && folder_path != ".") {
      vx_json_path /= folder_path;
    }
    vx_json_path /= "vx.json";

    if (!std::filesystem::exists(vx_json_path)) {
      return -1;
    }

    auto ftime = std::filesystem::last_write_time(vx_json_path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch());
    return millis.count();
  } catch (const std::exception& e) {
    VXCORE_LOG_ERROR("Failed to get JSON modified time for %s/%s: %s", filesystem_root.c_str(),
                     folder_path.c_str(), e.what());
    return -1;
  }
}

bool DbSyncManager::LoadFolderFromJson(const std::string& filesystem_root,
                                       const std::string& folder_path, int64_t parent_id) {
  // Construct path to vx.json
  std::filesystem::path vx_json_path =
      std::filesystem::path(filesystem_root) / "vx_notebook" / "notes";
  if (!folder_path.empty() && folder_path != ".") {
    vx_json_path /= folder_path;
  }
  vx_json_path /= "vx.json";

  // Load JSON
  nlohmann::json json;
  VxCoreError err = vxcore::LoadJsonFile(vx_json_path, json);
  if (err != VXCORE_OK) {
    last_error_ = "Failed to load vx.json: " + vx_json_path.string();
    VXCORE_LOG_ERROR("%s", last_error_.c_str());
    return false;
  }

  // Parse FolderConfig using core layer types
  vxcore::FolderConfig config;
  try {
    config = vxcore::FolderConfig::FromJson(json);
  } catch (const std::exception& e) {
    last_error_ = std::string("Failed to parse FolderConfig from JSON: ") + e.what();
    VXCORE_LOG_ERROR("%s", last_error_.c_str());
    return false;
  }

  // Convert to database layer types and create/update folder
  int64_t folder_db_id =
      file_db_->CreateOrUpdateFolder(config.id,              // uuid
                                     parent_id,              // parent_id
                                     config.name,            // name
                                     config.created_utc,     // created_utc
                                     config.modified_utc,    // modified_utc
                                     config.metadata.dump()  // metadata as JSON string
      );

  if (folder_db_id == -1) {
    last_error_ = "Failed to create/update folder in database";
    VXCORE_LOG_ERROR("%s", last_error_.c_str());
    return false;
  }

  // Insert/update files
  for (const auto& file_config : config.files) {
    int64_t file_db_id =
        file_db_->CreateOrUpdateFile(file_config.id,              // uuid
                                     folder_db_id,                // folder_id
                                     file_config.name,            // name
                                     file_config.created_utc,     // created_utc
                                     file_config.modified_utc,    // modified_utc
                                     file_config.metadata.dump()  // metadata as JSON string
        );

    if (file_db_id == -1) {
      VXCORE_LOG_WARN("Failed to create/update file: %s", file_config.name.c_str());
      continue;
    }

    // Insert tags for file
    for (const auto& tag_name : file_config.tags) {
      if (!file_db_->AddTagToFile(file_db_id, tag_name)) {
        VXCORE_LOG_WARN("Failed to add tag '%s' to file '%s'", tag_name.c_str(),
                        file_config.name.c_str());
      }
    }
  }

  VXCORE_LOG_DEBUG("Loaded folder from JSON: %s (id: %s, %zu files)", folder_path.c_str(),
                   config.id.c_str(), config.files.size());
  return true;
}

bool DbSyncManager::UpdateSyncState(int64_t folder_id, int64_t sync_time,
                                    int64_t json_modified_time) {
  if (!db_) {
    return false;
  }

  const char* sql =
      "UPDATE folders SET last_sync_utc = ?, metadata_file_modified_utc = ? WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    VXCORE_LOG_ERROR("Failed to prepare update sync state statement: %s", sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_int64(stmt, 1, sync_time);
  sqlite3_bind_int64(stmt, 2, json_modified_time);
  sqlite3_bind_int64(stmt, 3, folder_id);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

}  // namespace db
}  // namespace vxcore
