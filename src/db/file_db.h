#ifndef VXCORE_FILE_DB_H
#define VXCORE_FILE_DB_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Forward declare sqlite3
struct sqlite3;

namespace vxcore {
namespace db {

// File metadata structure (database layer)
struct DbFileRecord {
  int64_t id;         // Database auto-increment ID
  std::string uuid;   // UUID string from JSON
  int64_t folder_id;  // Foreign key to folders.id
  std::string name;
  int64_t created_utc;
  int64_t modified_utc;
  std::string metadata;  // JSON string
  std::vector<std::string> tags;
};

// Folder metadata structure (database layer)
struct DbFolderRecord {
  int64_t id;
  std::string uuid;
  int64_t parent_id;  // -1 for root folders (represents NULL)
  std::string name;
  int64_t created_utc;
  int64_t modified_utc;
  std::string metadata;
  int64_t last_sync_utc;               // -1 if never synced (represents NULL)
  int64_t metadata_file_modified_utc;  // -1 if never synced (represents NULL)
};

// File database operations (CRUD for files, folders, and file-tag relationships)
// NOT thread-safe: caller must ensure synchronization
class FileDb {
 public:
  explicit FileDb(sqlite3* db);
  ~FileDb() = default;

  // Disable copy/move
  FileDb(const FileDb&) = delete;
  FileDb& operator=(const FileDb&) = delete;

  // --- Folder Operations ---

  // Creates a new folder, returns folder_id or -1 on error
  // parent_id = -1 for root folders
  int64_t CreateFolder(int64_t parent_id, const std::string& name, int64_t created_utc,
                       int64_t modified_utc);

  // Creates or updates a folder with UUID (for sync), returns folder_id or -1 on error
  int64_t CreateOrUpdateFolder(const std::string& uuid, int64_t parent_id, const std::string& name,
                               int64_t created_utc, int64_t modified_utc,
                               const std::string& metadata);

  // Gets folder by id, returns nullopt if not found
  std::optional<DbFolderRecord> GetFolder(int64_t folder_id);

  // Gets folder by UUID string, returns nullopt if not found
  std::optional<DbFolderRecord> GetFolderByUuid(const std::string& uuid);

  // Gets folder by parent_id and name, returns nullopt if not found
  std::optional<DbFolderRecord> GetFolderByName(int64_t parent_id, const std::string& name);

  // Updates folder metadata, returns true on success
  bool UpdateFolder(int64_t folder_id, const std::string& name, int64_t modified_utc);

  // Deletes folder and all its contents recursively, returns true on success
  bool DeleteFolder(int64_t folder_id);

  // Lists all child folders of a parent folder
  // parent_id = -1 for root folders
  std::vector<DbFolderRecord> ListFolders(int64_t parent_id);

  // Computes relative path by walking parent chain
  // Returns empty string for root folders (parent_id = -1)
  std::string GetFolderPath(int64_t folder_id);

  // Moves folder to new parent. Returns true on success.
  // Fails if would create cycle (folder moved to itself or its descendants)
  bool MoveFolder(int64_t folder_id, int64_t new_parent_id);

  // Moves file to different folder. Returns true on success.
  bool MoveFile(int64_t file_id, int64_t new_folder_id);

  // --- File Operations ---

  // Creates a new file, returns file_id or -1 on error
  int64_t CreateFile(int64_t folder_id, const std::string& name, int64_t created_utc,
                     int64_t modified_utc, const std::vector<std::string>& tags);

  // Creates or updates a file with UUID (for sync), returns file_id or -1 on error
  int64_t CreateOrUpdateFile(const std::string& uuid, int64_t folder_id, const std::string& name,
                             int64_t created_utc, int64_t modified_utc,
                             const std::string& metadata);

  // Gets file by id, returns nullopt if not found
  std::optional<DbFileRecord> GetFile(int64_t file_id);

  // Gets file by UUID string, returns nullopt if not found
  std::optional<DbFileRecord> GetFileByUuid(const std::string& uuid);

  // Gets file by folder_id and name, returns nullopt if not found
  std::optional<DbFileRecord> GetFileByName(int64_t folder_id, const std::string& name);

  // Updates file metadata, returns true on success
  bool UpdateFile(int64_t file_id, const std::string& name, int64_t modified_utc,
                  const std::vector<std::string>& tags);

  // Deletes file, returns true on success
  bool DeleteFile(int64_t file_id);

  // Lists all files in a folder
  std::vector<DbFileRecord> ListFiles(int64_t folder_id);

  // --- File-Tag Relationship Operations ---

  // Adds a tag to a file by tag name (gets or creates tag via TagDb)
  // Note: Caller should use TagDb::GetOrCreateTag() for better control
  bool AddTagToFile(int64_t file_id, const std::string& tag_name);

  // Links a file to tags (replaces existing tags)
  bool SetFileTags(int64_t file_id, const std::vector<std::string>& tags);

  // Gets all tags for a file
  std::vector<std::string> GetFileTags(int64_t file_id);

  // Returns the last error message
  std::string GetLastError() const;

 private:
  sqlite3* db_;
};

}  // namespace db
}  // namespace vxcore

#endif  // VXCORE_FILE_DB_H
