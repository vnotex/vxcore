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
  int64_t folder_id;
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
// Uses folder path for lookup (traverses parent-child chain in DB)
// NOT thread-safe: caller must ensure synchronization
class DbSyncManager {
 public:
  DbSyncManager(sqlite3* db, FileDb* file_db);
  ~DbSyncManager() = default;

  // Disable copy/move
  DbSyncManager(const DbSyncManager&) = delete;
  DbSyncManager& operator=(const DbSyncManager&) = delete;

  // Checks if folder needs sync (compares JSON timestamp vs DB state)
  // Returns true if sync is needed (folder not in DB, never synced, or JSON changed)
  // folder_path: relative path to folder (e.g., "notes/subfolder")
  // filesystem_root: root path on disk where vx.json files are located
  bool NeedsSynchronization(const std::string& folder_path, const std::string& filesystem_root);

  // Synchronizes folder from JSON to database
  // Reads vx.json from filesystem and updates database
  // Returns SyncResult indicating success/failure
  SyncResult SynchronizeFolder(const std::string& folder_path, const std::string& filesystem_root);

  // Forces a full rebuild of folder in database (delete + re-import)
  SyncResult RebuildFolder(const std::string& folder_path, const std::string& filesystem_root);

  // Gets sync state for a folder by path, returns nullopt if folder not found or never synced
  std::optional<FolderSyncState> GetSyncState(const std::string& folder_path);

  // Clears sync state for a folder (e.g., when folder is removed)
  bool ClearSyncState(const std::string& folder_path);

  // Returns the last error message
  std::string GetLastError() const;

 private:
  // Internal helpers
  int64_t GetJsonModifiedTime(const std::string& filesystem_root, const std::string& folder_path);
  bool LoadFolderFromJson(const std::string& filesystem_root, const std::string& folder_path,
                          int64_t parent_id);
  bool UpdateSyncState(int64_t folder_id, int64_t sync_time, int64_t json_modified_time);

  sqlite3* db_;
  FileDb* file_db_;
  std::string last_error_;
};

}  // namespace db
}  // namespace vxcore

#endif  // VXCORE_SYNC_MANAGER_H
