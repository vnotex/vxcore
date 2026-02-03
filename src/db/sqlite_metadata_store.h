#ifndef VXCORE_SQLITE_METADATA_STORE_H
#define VXCORE_SQLITE_METADATA_STORE_H

#include <memory>
#include <string>
#include <unordered_map>

#include "core/metadata_store.h"

// Forward declarations
struct sqlite3;

namespace vxcore {
namespace db {

// Forward declarations
class DbManager;
class FileDb;
class TagDb;
class NotebookDb;

// SQLite-based implementation of MetadataStore
// Wraps DbManager, FileDb, TagDb to provide the MetadataStore interface
//
// Key design: Uses UUID strings for all public identifiers, but internally
// maps to int64_t database IDs for efficient SQLite operations.
//
// Thread safety: NOT thread-safe. Caller must ensure synchronization.
class SqliteMetadataStore : public MetadataStore {
 public:
  SqliteMetadataStore();
  ~SqliteMetadataStore() override;

  // Disable copy/move
  SqliteMetadataStore(const SqliteMetadataStore&) = delete;
  SqliteMetadataStore& operator=(const SqliteMetadataStore&) = delete;
  SqliteMetadataStore(SqliteMetadataStore&&) = delete;
  SqliteMetadataStore& operator=(SqliteMetadataStore&&) = delete;

  // --- Lifecycle ---
  bool Open(const std::string& db_path) override;
  void Close() override;
  bool IsOpen() const override;

  // --- Transaction Management ---
  bool BeginTransaction() override;
  bool CommitTransaction() override;
  bool RollbackTransaction() override;

  // --- Folder Operations ---
  bool CreateFolder(const StoreFolderRecord& folder) override;
  bool UpdateFolder(const std::string& folder_id, const std::string& name, int64_t modified_utc,
                    const std::string& metadata) override;
  bool DeleteFolder(const std::string& folder_id) override;
  std::optional<StoreFolderRecord> GetFolder(const std::string& folder_id) override;
  std::optional<StoreFolderRecord> GetFolderByPath(const std::string& path) override;
  std::vector<StoreFolderRecord> ListFolders(const std::string& parent_id) override;
  std::string GetFolderPath(const std::string& folder_id) override;
  bool MoveFolder(const std::string& folder_id, const std::string& new_parent_id) override;

  // --- File Operations ---
  bool CreateFile(const StoreFileRecord& file) override;
  bool UpdateFile(const std::string& file_id, const std::string& name, int64_t modified_utc,
                  const std::string& metadata) override;
  bool DeleteFile(const std::string& file_id) override;
  std::optional<StoreFileRecord> GetFile(const std::string& file_id) override;
  std::optional<StoreFileRecord> GetFileByPath(const std::string& path) override;
  std::vector<StoreFileRecord> ListFiles(const std::string& folder_id) override;
  bool MoveFile(const std::string& file_id, const std::string& new_folder_id) override;

  // --- Tag Definition Operations ---
  bool CreateOrUpdateTag(const StoreTagRecord& tag) override;
  bool DeleteTag(const std::string& tag_name) override;
  std::optional<StoreTagRecord> GetTag(const std::string& tag_name) override;
  std::vector<StoreTagRecord> ListTags() override;

  // --- File-Tag Association Operations ---
  bool SetFileTags(const std::string& file_id, const std::vector<std::string>& tags) override;
  bool AddTagToFile(const std::string& file_id, const std::string& tag_name) override;
  bool RemoveTagFromFile(const std::string& file_id, const std::string& tag_name) override;
  std::vector<std::string> GetFileTags(const std::string& file_id) override;

  // --- Tag Query Operations ---
  std::vector<StoreTagQueryResult> FindFilesByTagsOr(const std::vector<std::string>& tags) override;
  std::vector<StoreTagQueryResult> FindFilesByTagsAnd(
      const std::vector<std::string>& tags) override;
  std::vector<std::pair<std::string, int>> CountFilesByTag() override;

  // --- Sync/Recovery Operations ---
  std::optional<StoreSyncState> GetSyncState(const std::string& folder_id) override;
  bool UpdateSyncState(const std::string& folder_id, int64_t sync_time,
                       int64_t config_file_modified_utc) override;
  bool ClearSyncState(const std::string& folder_id) override;
  bool RebuildAll() override;

  // --- Iteration ---
  void IterateAllFiles(
      std::function<bool(const std::string&, const StoreFileRecord&)> callback) override;

  // --- Notebook Metadata ---
  std::optional<std::string> GetNotebookMetadata(const std::string& key) override;
  bool SetNotebookMetadata(const std::string& key, const std::string& value) override;

  // --- Error Handling ---
  std::string GetLastError() const override;

 private:
  // Internal helpers for UUID <-> int64_t ID mapping
  int64_t GetFolderDbId(const std::string& folder_uuid);
  int64_t GetFileDbId(const std::string& file_uuid);
  std::string GetFolderUuid(int64_t folder_db_id);
  std::string GetFileUuid(int64_t file_db_id);

  // Convert between DB records and Store records
  StoreFolderRecord ToStoreFolderRecord(const struct DbFolderRecord& db_record);
  StoreFileRecord ToStoreFileRecord(const struct DbFileRecord& db_record);

  std::unique_ptr<DbManager> db_manager_;
  std::unique_ptr<FileDb> file_db_;
  std::unique_ptr<TagDb> tag_db_;
  std::unique_ptr<NotebookDb> notebook_db_;
  mutable std::string last_error_;
};

}  // namespace db
}  // namespace vxcore

#endif  // VXCORE_SQLITE_METADATA_STORE_H
