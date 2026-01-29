#ifndef VXCORE_SYNC_MANAGER_H
#define VXCORE_SYNC_MANAGER_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// Forward declare sqlite3
struct sqlite3;

namespace vxcore {
namespace db {

// Forward declarations
class FileDb;

// Folder sync state
struct FolderSyncState {
  std::string folder_uuid;
  int64_t last_sync_utc;
  int64_t metadata_file_modified_utc;
};

// Sync operation result
enum class SyncResult {
  kSuccess,         // Sync completed successfully
  kNoChanges,       // No changes detected
  kJsonNotFound,    // vx.json file not found
  kJsonParseError,  // Failed to parse vx.json
  kDatabaseError,   // Database operation failed
  kFileSystemError  // File system error
};

// Database sync manager - handles JSON ↔ DB synchronization
// NOT thread-safe: caller must ensure synchronization
class DbSyncManager {
 public:
  DbSyncManager(sqlite3* db, FileDb* file_db);
  ~DbSyncManager() = default;

  // Disable copy/move
  DbSyncManager(const DbSyncManager&) = delete;
  DbSyncManager& operator=(const DbSyncManager&) = delete;

  // Checks if folder needs sync (compares JSON timestamp vs DB state)
  // Returns true if sync is needed
  // folder_uuid: UUID of folder in database, filesystem_path: path to folder on disk
  bool NeedsSynchronization(const std::string& folder_uuid, const std::string& filesystem_path);

  // Synchronizes folder from JSON to database
  // Reads vx.json from filesystem_path and updates database
  // Returns SyncResult indicating success/failure
  SyncResult SynchronizeFolder(const std::string& folder_uuid, const std::string& filesystem_path);

  // Forces a full rebuild of folder in database (delete + re-import)
  SyncResult RebuildFolder(const std::string& folder_uuid, const std::string& filesystem_path);

  // Gets sync state for a folder by UUID, returns nullopt if never synced
  std::optional<FolderSyncState> GetSyncState(const std::string& folder_uuid);

  // Clears sync state for a folder (e.g., when folder is removed)
  bool ClearSyncState(const std::string& folder_uuid);

  // Returns the last error message
  std::string GetLastError() const;

 private:
  // Internal helpers
  int64_t GetJsonModifiedTime(const std::string& filesystem_path);
  bool LoadFolderFromJson(const std::string& filesystem_path, int64_t parent_id);
  bool UpdateSyncState(const std::string& folder_uuid, int64_t sync_time,
                       int64_t json_modified_time);

  sqlite3* db_;
  FileDb* file_db_;
  std::string last_error_;
};

}  // namespace db
}  // namespace vxcore

#endif  // VXCORE_SYNC_MANAGER_H
