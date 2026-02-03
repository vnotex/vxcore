#ifndef VXCORE_METADATA_STORE_H
#define VXCORE_METADATA_STORE_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vxcore {

// Forward declarations
struct FileRecord;
struct FolderRecord;
struct FolderConfig;

// Metadata store record structures (storage-agnostic)
// These mirror the core types but are used for store operations

struct StoreFolderRecord {
  std::string id;         // UUID
  std::string parent_id;  // Parent folder UUID, empty for root folders
  std::string name;
  int64_t created_utc;
  int64_t modified_utc;
  std::string metadata;  // JSON string
};

struct StoreFileRecord {
  std::string id;         // UUID
  std::string folder_id;  // Parent folder UUID
  std::string name;
  int64_t created_utc;
  int64_t modified_utc;
  std::string metadata;  // JSON string
  std::vector<std::string> tags;
};

struct StoreTagRecord {
  std::string name;
  std::string parent_name;  // Parent tag name, empty for root tags
  std::string metadata;     // JSON string
};

// Tag query result
struct StoreTagQueryResult {
  std::string file_id;
  std::string folder_id;
  std::string file_name;
  std::string file_path;  // Full relative path (folder_path/file_name)
  std::vector<std::string> tags;
};

// Sync state for a folder
struct StoreSyncState {
  std::string folder_id;
  int64_t last_sync_utc;
  int64_t config_file_modified_utc;  // vx.json modification time
};

// Sync result codes
enum class SyncResultCode {
  kSuccess,
  kNoChanges,
  kConfigNotFound,
  kConfigParseError,
  kStoreError,
  kFileSystemError
};

// Abstract interface for metadata storage
// This is a write-through cache layer - config files remain ground truth
// Storage implementations (SQLite, etc.) provide fast queries
//
// Thread safety: NOT thread-safe. Caller must ensure synchronization.
class MetadataStore {
 public:
  virtual ~MetadataStore() = default;

  // --- Lifecycle ---

  // Opens/initializes the store for a notebook
  // db_path: path to store the metadata (e.g., database file)
  virtual bool Open(const std::string& db_path) = 0;

  // Closes the store
  virtual void Close() = 0;

  // Returns true if store is open and ready
  virtual bool IsOpen() const = 0;

  // --- Transaction Management ---
  // Use transactions to batch multiple operations atomically

  virtual bool BeginTransaction() = 0;
  virtual bool CommitTransaction() = 0;
  virtual bool RollbackTransaction() = 0;

  // --- Folder Operations ---

  // Creates a folder in the store
  // Returns true on success
  virtual bool CreateFolder(const StoreFolderRecord& folder) = 0;

  // Updates folder metadata
  virtual bool UpdateFolder(const std::string& folder_id, const std::string& name,
                            int64_t modified_utc, const std::string& metadata) = 0;

  // Deletes a folder and all its contents (files and subfolders)
  virtual bool DeleteFolder(const std::string& folder_id) = 0;

  // Gets folder by UUID
  virtual std::optional<StoreFolderRecord> GetFolder(const std::string& folder_id) = 0;

  // Gets folder by path (e.g., "notes/subfolder")
  // Empty path returns nullopt (root has no folder record)
  virtual std::optional<StoreFolderRecord> GetFolderByPath(const std::string& path) = 0;

  // Lists all child folders of a parent
  // Empty parent_id = root folders
  virtual std::vector<StoreFolderRecord> ListFolders(const std::string& parent_id) = 0;

  // Gets the relative path for a folder by traversing parent chain
  virtual std::string GetFolderPath(const std::string& folder_id) = 0;

  // Moves folder to new parent
  virtual bool MoveFolder(const std::string& folder_id, const std::string& new_parent_id) = 0;

  // --- File Operations ---

  // Creates a file in the store
  virtual bool CreateFile(const StoreFileRecord& file) = 0;

  // Updates file metadata (name, modified_utc, metadata)
  virtual bool UpdateFile(const std::string& file_id, const std::string& name, int64_t modified_utc,
                          const std::string& metadata) = 0;

  // Deletes a file
  virtual bool DeleteFile(const std::string& file_id) = 0;

  // Gets file by UUID
  virtual std::optional<StoreFileRecord> GetFile(const std::string& file_id) = 0;

  // Gets file by path (e.g., "notes/subfolder/myfile.md")
  virtual std::optional<StoreFileRecord> GetFileByPath(const std::string& path) = 0;

  // Lists all files in a folder
  virtual std::vector<StoreFileRecord> ListFiles(const std::string& folder_id) = 0;

  // Moves file to different folder
  virtual bool MoveFile(const std::string& file_id, const std::string& new_folder_id) = 0;

  // --- Tag Definition Operations ---
  // Tags are defined at notebook level, stored in NotebookConfig
  // These methods sync tag definitions to the store for query support

  // Creates or updates a tag definition
  virtual bool CreateOrUpdateTag(const StoreTagRecord& tag) = 0;

  // Deletes a tag definition (also removes from all files)
  virtual bool DeleteTag(const std::string& tag_name) = 0;

  // Gets a tag by name
  virtual std::optional<StoreTagRecord> GetTag(const std::string& tag_name) = 0;

  // Lists all tag definitions
  virtual std::vector<StoreTagRecord> ListTags() = 0;

  // --- File-Tag Association Operations ---

  // Sets all tags for a file (replaces existing)
  virtual bool SetFileTags(const std::string& file_id, const std::vector<std::string>& tags) = 0;

  // Adds a single tag to a file
  virtual bool AddTagToFile(const std::string& file_id, const std::string& tag_name) = 0;

  // Removes a single tag from a file
  virtual bool RemoveTagFromFile(const std::string& file_id, const std::string& tag_name) = 0;

  // Gets all tags for a file
  virtual std::vector<std::string> GetFileTags(const std::string& file_id) = 0;

  // --- Tag Query Operations ---

  // Finds files with ANY of the given tags (OR logic)
  virtual std::vector<StoreTagQueryResult> FindFilesByTagsOr(
      const std::vector<std::string>& tags) = 0;

  // Finds files with ALL of the given tags (AND logic)
  virtual std::vector<StoreTagQueryResult> FindFilesByTagsAnd(
      const std::vector<std::string>& tags) = 0;

  // Counts files for each tag
  virtual std::vector<std::pair<std::string, int>> CountFilesByTag() = 0;

  // --- Sync/Recovery Operations ---
  // These methods support rebuilding the store from config files

  // Gets sync state for a folder
  virtual std::optional<StoreSyncState> GetSyncState(const std::string& folder_id) = 0;

  // Updates sync state after syncing a folder
  virtual bool UpdateSyncState(const std::string& folder_id, int64_t sync_time,
                               int64_t config_file_modified_utc) = 0;

  // Clears sync state (e.g., when folder is deleted)
  virtual bool ClearSyncState(const std::string& folder_id) = 0;

  // Rebuilds entire store (drops all data and re-initializes schema)
  // WARNING: This deletes all cached data!
  virtual bool RebuildAll() = 0;

  // --- Iteration ---

  // Iterates all files in the store
  // Callback receives (file_path, file_record), returns false to stop iteration
  virtual void IterateAllFiles(
      std::function<bool(const std::string&, const StoreFileRecord&)> callback) = 0;

  // --- Notebook Metadata ---
  // Key-value store for notebook-level metadata (e.g., tags_synced_utc)
  virtual std::optional<std::string> GetNotebookMetadata(const std::string& key) = 0;
  virtual bool SetNotebookMetadata(const std::string& key, const std::string& value) = 0;

  // --- Error Handling ---

  // Returns the last error message
  virtual std::string GetLastError() const = 0;
};

}  // namespace vxcore

#endif  // VXCORE_METADATA_STORE_H
