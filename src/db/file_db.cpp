#include "file_db.h"

#include <sqlite3.h>

#include <chrono>

#include "tag_db.h"

namespace vxcore {
namespace db {

FileDb::FileDb(sqlite3* db) : db_(db) {}

std::string FileDb::GetLastError() const { return sqlite3_errmsg(db_); }

// --- Folder Operations ---

int64_t FileDb::CreateFolder(int64_t parent_id, const std::string& name, int64_t created_utc,
                             int64_t modified_utc) {
  // First insert with placeholder values
  const char* sql =
      "INSERT INTO folders (parent_id, name, created_utc, modified_utc, uuid, metadata) "
      "VALUES (?, ?, ?, ?, 'temp', '');";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return -1;
  }

  // Bind parameters (parent_id = -1 means NULL for root folders)
  if (parent_id == -1) {
    sqlite3_bind_null(stmt, 1);
  } else {
    sqlite3_bind_int64(stmt, 1, parent_id);
  }
  sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, created_utc);
  sqlite3_bind_int64(stmt, 4, modified_utc);

  rc = sqlite3_step(stmt);
  int64_t folder_id = -1;
  if (rc == SQLITE_DONE) {
    folder_id = sqlite3_last_insert_rowid(db_);
  }
  sqlite3_finalize(stmt);

  if (folder_id == -1) {
    return -1;
  }

  // Update with ID-based unique values
  const char* update_sql = "UPDATE folders SET uuid = ? WHERE id = ?;";
  rc = sqlite3_prepare_v2(db_, update_sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return -1;
  }

  std::string uuid = "_folder_" + std::to_string(folder_id);
  sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, folder_id);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return (rc == SQLITE_DONE) ? folder_id : -1;
}

int64_t FileDb::CreateOrUpdateFolder(const std::string& uuid, int64_t parent_id,
                                     const std::string& name, int64_t created_utc,
                                     int64_t modified_utc, const std::string& metadata) {
  const char* sql =
      "INSERT OR REPLACE INTO folders (uuid, parent_id, name, created_utc, modified_utc, "
      "metadata, last_sync_utc, metadata_file_modified_utc) "
      "VALUES (?, ?, ?, ?, ?, ?, NULL, NULL);";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return -1;
  }

  sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
  if (parent_id == -1) {
    sqlite3_bind_null(stmt, 2);
  } else {
    sqlite3_bind_int64(stmt, 2, parent_id);
  }
  sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, created_utc);
  sqlite3_bind_int64(stmt, 5, modified_utc);
  sqlite3_bind_text(stmt, 6, metadata.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  int64_t folder_id = -1;
  if (rc == SQLITE_DONE) {
    folder_id = sqlite3_last_insert_rowid(db_);
  }

  sqlite3_finalize(stmt);
  return folder_id;
}

std::optional<DbFolderRecord> FileDb::GetFolder(int64_t folder_id) {
  const char* sql =
      "SELECT id, uuid, parent_id, name, created_utc, modified_utc, metadata, "
      "last_sync_utc, metadata_file_modified_utc FROM folders "
      "WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return std::nullopt;
  }

  sqlite3_bind_int64(stmt, 1, folder_id);

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  DbFolderRecord folder;
  folder.id = sqlite3_column_int64(stmt, 0);
  folder.uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  folder.parent_id =
      sqlite3_column_type(stmt, 2) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 2);
  folder.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  folder.created_utc = sqlite3_column_int64(stmt, 4);
  folder.modified_utc = sqlite3_column_int64(stmt, 5);
  folder.metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  folder.last_sync_utc =
      sqlite3_column_type(stmt, 7) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 7);
  folder.metadata_file_modified_utc =
      sqlite3_column_type(stmt, 8) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 8);

  sqlite3_finalize(stmt);
  return folder;
}

std::optional<DbFolderRecord> FileDb::GetFolderByUuid(const std::string& uuid) {
  const char* sql =
      "SELECT id, uuid, parent_id, name, created_utc, modified_utc, metadata, "
      "last_sync_utc, metadata_file_modified_utc FROM folders "
      "WHERE uuid = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return std::nullopt;
  }

  sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  DbFolderRecord folder;
  folder.id = sqlite3_column_int64(stmt, 0);
  folder.uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  folder.parent_id =
      sqlite3_column_type(stmt, 2) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 2);
  folder.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  folder.created_utc = sqlite3_column_int64(stmt, 4);
  folder.modified_utc = sqlite3_column_int64(stmt, 5);
  folder.metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  folder.last_sync_utc =
      sqlite3_column_type(stmt, 7) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 7);
  folder.metadata_file_modified_utc =
      sqlite3_column_type(stmt, 8) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 8);

  sqlite3_finalize(stmt);
  return folder;
}

std::optional<DbFolderRecord> FileDb::GetFolderByName(int64_t parent_id, const std::string& name) {
  const char* sql;
  if (parent_id == -1) {
    sql =
        "SELECT id, uuid, parent_id, name, created_utc, modified_utc, metadata, "
        "last_sync_utc, metadata_file_modified_utc FROM folders "
        "WHERE parent_id IS NULL AND name = ?;";
  } else {
    sql =
        "SELECT id, uuid, parent_id, name, created_utc, modified_utc, metadata, "
        "last_sync_utc, metadata_file_modified_utc FROM folders "
        "WHERE parent_id = ? AND name = ?;";
  }

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return std::nullopt;
  }

  if (parent_id == -1) {
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_int64(stmt, 1, parent_id);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  }

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  DbFolderRecord folder;
  folder.id = sqlite3_column_int64(stmt, 0);
  folder.uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  folder.parent_id =
      sqlite3_column_type(stmt, 2) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 2);
  folder.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  folder.created_utc = sqlite3_column_int64(stmt, 4);
  folder.modified_utc = sqlite3_column_int64(stmt, 5);
  folder.metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  folder.last_sync_utc =
      sqlite3_column_type(stmt, 7) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 7);
  folder.metadata_file_modified_utc =
      sqlite3_column_type(stmt, 8) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 8);

  sqlite3_finalize(stmt);
  return folder;
}

bool FileDb::UpdateFolder(int64_t folder_id, const std::string& name, int64_t modified_utc) {
  const char* sql = "UPDATE folders SET name = ?, modified_utc = ? WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, modified_utc);
  sqlite3_bind_int64(stmt, 3, folder_id);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool FileDb::DeleteFolder(int64_t folder_id) {
  // Foreign key cascade will handle deletion of children
  const char* sql = "DELETE FROM folders WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_int64(stmt, 1, folder_id);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

std::vector<DbFolderRecord> FileDb::ListFolders(int64_t parent_id) {
  const char* sql;
  if (parent_id == -1) {
    sql =
        "SELECT id, uuid, parent_id, name, created_utc, modified_utc, metadata, "
        "last_sync_utc, metadata_file_modified_utc FROM folders "
        "WHERE parent_id IS NULL ORDER BY name;";
  } else {
    sql =
        "SELECT id, uuid, parent_id, name, created_utc, modified_utc, metadata, "
        "last_sync_utc, metadata_file_modified_utc FROM folders "
        "WHERE parent_id = ? ORDER BY name;";
  }

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return {};
  }

  if (parent_id != -1) {
    sqlite3_bind_int64(stmt, 1, parent_id);
  }

  std::vector<DbFolderRecord> folders;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    DbFolderRecord folder;
    folder.id = sqlite3_column_int64(stmt, 0);
    folder.uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    folder.parent_id =
        sqlite3_column_type(stmt, 2) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 2);
    folder.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    folder.created_utc = sqlite3_column_int64(stmt, 4);
    folder.modified_utc = sqlite3_column_int64(stmt, 5);
    folder.metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    folder.last_sync_utc =
        sqlite3_column_type(stmt, 7) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 7);
    folder.metadata_file_modified_utc =
        sqlite3_column_type(stmt, 8) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 8);
    folders.push_back(folder);
  }

  sqlite3_finalize(stmt);
  return folders;
}

std::string FileDb::GetFolderPath(int64_t folder_id) {
  std::vector<std::string> path_parts;

  int64_t current_id = folder_id;
  while (current_id != -1) {
    auto folder = GetFolder(current_id);
    if (!folder.has_value()) {
      break;
    }
    path_parts.push_back(folder->name);
    current_id = folder->parent_id;
  }

  // Build path from root to leaf (reverse the collected parts)
  std::string result;
  for (auto it = path_parts.rbegin(); it != path_parts.rend(); ++it) {
    if (!result.empty()) {
      result += "/";
    }
    result += *it;
  }

  return result;
}

bool FileDb::MoveFolder(int64_t folder_id, int64_t new_parent_id) {
  // Check for cycle: new_parent_id cannot be folder_id or any descendant of folder_id
  if (new_parent_id == folder_id) {
    return false;  // Cannot move folder to itself
  }

  // Check if new_parent_id is a descendant of folder_id
  int64_t current = new_parent_id;
  while (current != -1) {
    auto parent = GetFolder(current);
    if (!parent.has_value()) {
      break;
    }
    if (parent->parent_id == folder_id) {
      return false;  // Would create cycle
    }
    current = parent->parent_id;
  }

  const char* sql = "UPDATE folders SET parent_id = ?, modified_utc = ? WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }

  if (new_parent_id == -1) {
    sqlite3_bind_null(stmt, 1);
  } else {
    sqlite3_bind_int64(stmt, 1, new_parent_id);
  }

  // Get current timestamp for modified_utc
  auto now = std::chrono::system_clock::now();
  auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
  sqlite3_bind_int64(stmt, 2, millis.count());
  sqlite3_bind_int64(stmt, 3, folder_id);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool FileDb::MoveFile(int64_t file_id, int64_t new_folder_id) {
  const char* sql = "UPDATE files SET folder_id = ?, modified_utc = ? WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_int64(stmt, 1, new_folder_id);

  // Get current timestamp for modified_utc
  auto now = std::chrono::system_clock::now();
  auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
  sqlite3_bind_int64(stmt, 2, millis.count());
  sqlite3_bind_int64(stmt, 3, file_id);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

// --- File Operations ---

int64_t FileDb::CreateFile(int64_t folder_id, const std::string& name, int64_t created_utc,
                           int64_t modified_utc, const std::vector<std::string>& tags) {
  // First insert with placeholder UUID
  const char* sql =
      "INSERT INTO files (folder_id, name, created_utc, modified_utc, uuid, metadata) "
      "VALUES (?, ?, ?, ?, 'temp', '');";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return -1;
  }

  sqlite3_bind_int64(stmt, 1, folder_id);
  sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, created_utc);
  sqlite3_bind_int64(stmt, 4, modified_utc);

  rc = sqlite3_step(stmt);
  int64_t file_id = -1;
  if (rc == SQLITE_DONE) {
    file_id = sqlite3_last_insert_rowid(db_);
  }
  sqlite3_finalize(stmt);

  if (file_id == -1) {
    return -1;
  }

  // Update with ID-based unique UUID
  const char* update_sql = "UPDATE files SET uuid = ? WHERE id = ?;";
  rc = sqlite3_prepare_v2(db_, update_sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return -1;
  }

  std::string uuid = "_file_" + std::to_string(file_id);
  sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, file_id);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    return -1;
  }

  // Add tags
  if (!tags.empty()) {
    SetFileTags(file_id, tags);
  }

  return file_id;
}

int64_t FileDb::CreateOrUpdateFile(const std::string& uuid, int64_t folder_id,
                                   const std::string& name, int64_t created_utc,
                                   int64_t modified_utc, const std::string& metadata) {
  const char* sql =
      "INSERT OR REPLACE INTO files (uuid, folder_id, name, created_utc, modified_utc, metadata) "
      "VALUES (?, ?, ?, ?, ?, ?);";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return -1;
  }

  sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, folder_id);
  sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, created_utc);
  sqlite3_bind_int64(stmt, 5, modified_utc);
  sqlite3_bind_text(stmt, 6, metadata.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  int64_t file_id = -1;
  if (rc == SQLITE_DONE) {
    file_id = sqlite3_last_insert_rowid(db_);
  }

  sqlite3_finalize(stmt);
  return file_id;
}

std::optional<DbFileRecord> FileDb::GetFile(int64_t file_id) {
  const char* sql =
      "SELECT id, uuid, folder_id, name, created_utc, modified_utc, metadata FROM files "
      "WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return std::nullopt;
  }

  sqlite3_bind_int64(stmt, 1, file_id);

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  DbFileRecord file;
  file.id = sqlite3_column_int64(stmt, 0);
  file.uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  file.folder_id = sqlite3_column_int64(stmt, 2);
  file.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  file.created_utc = sqlite3_column_int64(stmt, 4);
  file.modified_utc = sqlite3_column_int64(stmt, 5);
  file.metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  file.tags = GetFileTags(file_id);

  sqlite3_finalize(stmt);
  return file;
}

std::optional<DbFileRecord> FileDb::GetFileByUuid(const std::string& uuid) {
  const char* sql =
      "SELECT id, uuid, folder_id, name, created_utc, modified_utc, metadata FROM files "
      "WHERE uuid = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return std::nullopt;
  }

  sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  DbFileRecord file;
  file.id = sqlite3_column_int64(stmt, 0);
  file.uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  file.folder_id = sqlite3_column_int64(stmt, 2);
  file.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  file.created_utc = sqlite3_column_int64(stmt, 4);
  file.modified_utc = sqlite3_column_int64(stmt, 5);
  file.metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  file.tags = GetFileTags(file.id);

  sqlite3_finalize(stmt);
  return file;
}

std::optional<DbFileRecord> FileDb::GetFileByName(int64_t folder_id, const std::string& name) {
  const char* sql =
      "SELECT id, uuid, folder_id, name, created_utc, modified_utc, metadata FROM files "
      "WHERE folder_id = ? AND name = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return std::nullopt;
  }

  sqlite3_bind_int64(stmt, 1, folder_id);
  sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  DbFileRecord file;
  file.id = sqlite3_column_int64(stmt, 0);
  file.uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  file.folder_id = sqlite3_column_int64(stmt, 2);
  file.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  file.created_utc = sqlite3_column_int64(stmt, 4);
  file.modified_utc = sqlite3_column_int64(stmt, 5);
  file.metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  file.tags = GetFileTags(file.id);

  sqlite3_finalize(stmt);
  return file;
}

bool FileDb::UpdateFile(int64_t file_id, const std::string& name, int64_t modified_utc,
                        const std::vector<std::string>& tags) {
  const char* sql = "UPDATE files SET name = ?, modified_utc = ? WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, modified_utc);
  sqlite3_bind_int64(stmt, 3, file_id);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    return false;
  }

  // Update tags
  return SetFileTags(file_id, tags);
}

bool FileDb::DeleteFile(int64_t file_id) {
  // Delete file (cascade will handle file_tags)
  const char* sql = "DELETE FROM files WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_int64(stmt, 1, file_id);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

std::vector<DbFileRecord> FileDb::ListFiles(int64_t folder_id) {
  const char* sql =
      "SELECT id, uuid, folder_id, name, created_utc, modified_utc, metadata FROM files "
      "WHERE folder_id = ? ORDER BY name;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return {};
  }

  sqlite3_bind_int64(stmt, 1, folder_id);

  std::vector<DbFileRecord> files;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    DbFileRecord file;
    file.id = sqlite3_column_int64(stmt, 0);
    file.uuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    file.folder_id = sqlite3_column_int64(stmt, 2);
    file.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    file.created_utc = sqlite3_column_int64(stmt, 4);
    file.modified_utc = sqlite3_column_int64(stmt, 5);
    file.metadata = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    file.tags = GetFileTags(file.id);
    files.push_back(file);
  }

  sqlite3_finalize(stmt);
  return files;
}

// --- File-Tag Relationship Operations ---

bool FileDb::AddTagToFile(int64_t file_id, const std::string& tag_name) {
  TagDb tag_db(db_);
  int64_t tag_id = tag_db.GetOrCreateTag(tag_name);
  if (tag_id == -1) {
    return false;
  }

  const char* sql = "INSERT OR IGNORE INTO file_tags (file_id, tag_id) VALUES (?, ?);";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_int64(stmt, 1, file_id);
  sqlite3_bind_int64(stmt, 2, tag_id);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool FileDb::SetFileTags(int64_t file_id, const std::vector<std::string>& tags) {
  // Delete existing tags
  const char* delete_sql = "DELETE FROM file_tags WHERE file_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, delete_sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_int64(stmt, 1, file_id);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    return false;
  }

  // Insert new tags
  const char* insert_sql = "INSERT INTO file_tags (file_id, tag_id) VALUES (?, ?);";
  TagDb tag_db(db_);

  for (const auto& tag_name : tags) {
    int64_t tag_id = tag_db.GetOrCreateTag(tag_name);
    if (tag_id == -1) {
      return false;
    }

    rc = sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
      return false;
    }

    sqlite3_bind_int64(stmt, 1, file_id);
    sqlite3_bind_int64(stmt, 2, tag_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
      return false;
    }
  }

  return true;
}

std::vector<std::string> FileDb::GetFileTags(int64_t file_id) {
  const char* sql =
      "SELECT t.name FROM tags t "
      "JOIN file_tags ft ON t.id = ft.tag_id "
      "WHERE ft.file_id = ? ORDER BY t.name;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return {};
  }

  sqlite3_bind_int64(stmt, 1, file_id);

  std::vector<std::string> tags;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    tags.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
  }

  sqlite3_finalize(stmt);
  return tags;
}

}  // namespace db
}  // namespace vxcore
