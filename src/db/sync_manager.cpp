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

bool DbSyncManager::NeedsSynchronization(const std::string& folder_uuid,
                                         const std::string& filesystem_path) {
  auto sync_state = GetSyncState(folder_uuid);
  if (!sync_state.has_value()) {
    return true;  // Never synced before
  }

  int64_t json_modified = GetJsonModifiedTime(filesystem_path);
  if (json_modified <= 0) {
    return false;  // No vx.json file
  }

  return json_modified > sync_state->metadata_file_modified_utc;
}

SyncResult DbSyncManager::SynchronizeFolder(const std::string& folder_uuid,
                                            const std::string& filesystem_path) {
  if (!db_ || !file_db_) {
    last_error_ = "Database or FileDb not initialized";
    return SyncResult::kDatabaseError;
  }

  VXCORE_LOG_DEBUG("Synchronizing folder: uuid=%s, path=%s", folder_uuid.c_str(),
                   filesystem_path.c_str());

  // Get JSON modification time
  int64_t json_modified = GetJsonModifiedTime(filesystem_path);
  if (json_modified <= 0) {
    last_error_ = "vx.json file not found or inaccessible";
    return SyncResult::kJsonNotFound;
  }

  // Check if sync needed
  auto sync_state = GetSyncState(folder_uuid);
  if (sync_state.has_value() && json_modified <= sync_state->metadata_file_modified_utc) {
    VXCORE_LOG_DEBUG("No changes detected for folder: %s", folder_uuid.c_str());
    return SyncResult::kNoChanges;
  }

  // Load and sync folder
  int64_t sync_time = GetCurrentTimestampMillis();
  if (!LoadFolderFromJson(filesystem_path, -1)) {
    return SyncResult::kDatabaseError;
  }

  // Update sync state
  if (!UpdateSyncState(folder_uuid, sync_time, json_modified)) {
    VXCORE_LOG_WARN("Failed to update sync state for: %s", folder_uuid.c_str());
  }

  VXCORE_LOG_INFO("Successfully synchronized folder: uuid=%s", folder_uuid.c_str());
  return SyncResult::kSuccess;
}

SyncResult DbSyncManager::RebuildFolder(const std::string& folder_uuid,
                                        const std::string& filesystem_path) {
  if (!db_ || !file_db_) {
    last_error_ = "Database or FileDb not initialized";
    return SyncResult::kDatabaseError;
  }

  VXCORE_LOG_INFO("Rebuilding folder: uuid=%s", folder_uuid.c_str());

  // Get JSON modification time
  int64_t json_modified = GetJsonModifiedTime(filesystem_path);
  if (json_modified <= 0) {
    last_error_ = "vx.json file not found or inaccessible";
    return SyncResult::kJsonNotFound;
  }

  // Delete existing folder data from DB (lookup by uuid)
  auto folder_record = file_db_->GetFolderByUuid(folder_uuid);
  if (folder_record.has_value()) {
    if (!file_db_->DeleteFolder(folder_record->id)) {
      last_error_ = "Failed to delete existing folder data";
      return SyncResult::kDatabaseError;
    }
  }

  // Clear sync state
  ClearSyncState(folder_uuid);

  // Re-import from JSON
  int64_t sync_time = GetCurrentTimestampMillis();
  if (!LoadFolderFromJson(filesystem_path, -1)) {
    return SyncResult::kDatabaseError;
  }

  // Update sync state
  if (!UpdateSyncState(folder_uuid, sync_time, json_modified)) {
    VXCORE_LOG_WARN("Failed to update sync state for: %s", folder_uuid.c_str());
  }

  VXCORE_LOG_INFO("Successfully rebuilt folder: uuid=%s", folder_uuid.c_str());
  return SyncResult::kSuccess;
}

std::optional<FolderSyncState> DbSyncManager::GetSyncState(const std::string& folder_uuid) {
  if (!db_) {
    return std::nullopt;
  }

  const char* sql = "SELECT last_sync_utc, metadata_file_modified_utc FROM folders WHERE uuid = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    VXCORE_LOG_ERROR("Failed to prepare get sync state statement: %s", sqlite3_errmsg(db_));
    return std::nullopt;
  }

  sqlite3_bind_text(stmt, 1, folder_uuid.c_str(), -1, SQLITE_TRANSIENT);

  FolderSyncState state;
  state.folder_uuid = folder_uuid;

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    // Handle NULL values (-1 represents NULL)
    state.last_sync_utc =
        sqlite3_column_type(stmt, 0) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 0);
    state.metadata_file_modified_utc =
        sqlite3_column_type(stmt, 1) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 1);
    sqlite3_finalize(stmt);

    // Return nullopt if both values are NULL (never synced)
    if (state.last_sync_utc == -1 && state.metadata_file_modified_utc == -1) {
      return std::nullopt;
    }

    return state;
  }

  sqlite3_finalize(stmt);
  return std::nullopt;
}

bool DbSyncManager::ClearSyncState(const std::string& folder_uuid) {
  if (!db_) {
    return false;
  }

  const char* sql =
      "UPDATE folders SET last_sync_utc = NULL, metadata_file_modified_utc = NULL WHERE uuid = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    VXCORE_LOG_ERROR("Failed to prepare clear sync state statement: %s", sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, folder_uuid.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

std::string DbSyncManager::GetLastError() const { return last_error_; }

// Private helper methods

int64_t DbSyncManager::GetJsonModifiedTime(const std::string& filesystem_path) {
  try {
    std::filesystem::path vx_json_path =
        std::filesystem::path(filesystem_path) / "vx_notebook" / "notes" / "vx.json";

    if (!std::filesystem::exists(vx_json_path)) {
      return -1;
    }

    auto ftime = std::filesystem::last_write_time(vx_json_path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch());
    return millis.count();
  } catch (const std::exception& e) {
    VXCORE_LOG_ERROR("Failed to get JSON modified time for %s: %s", filesystem_path.c_str(),
                     e.what());
    return -1;
  }
}

bool DbSyncManager::LoadFolderFromJson(const std::string& filesystem_path, int64_t parent_id) {
  // Construct path to vx.json
  std::filesystem::path vx_json_path =
      std::filesystem::path(filesystem_path) / "vx_notebook" / "notes" / "vx.json";

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

  VXCORE_LOG_DEBUG("Loaded folder from JSON: %s (id: %s, %zu files)", filesystem_path.c_str(),
                   config.id.c_str(), config.files.size());
  return true;
}

bool DbSyncManager::UpdateSyncState(const std::string& folder_uuid, int64_t sync_time,
                                    int64_t json_modified_time) {
  if (!db_) {
    return false;
  }

  const char* sql =
      "UPDATE folders SET last_sync_utc = ?, metadata_file_modified_utc = ? WHERE uuid = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    VXCORE_LOG_ERROR("Failed to prepare update sync state statement: %s", sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_int64(stmt, 1, sync_time);
  sqlite3_bind_int64(stmt, 2, json_modified_time);
  sqlite3_bind_text(stmt, 3, folder_uuid.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

}  // namespace db
}  // namespace vxcore
