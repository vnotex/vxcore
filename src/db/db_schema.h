#ifndef VXCORE_DB_SCHEMA_H
#define VXCORE_DB_SCHEMA_H

#include <string>

namespace vxcore {
namespace db {

// SQL schema definitions for vxcore database
namespace schema {

// Schema version for migration tracking
constexpr int kCurrentSchemaVersion = 3;

// Folders table: stores folder hierarchy
// parent_id references folders(id) - NULL for root folders
// uuid is the string ID from JSON files (from FolderConfig.id)
inline constexpr const char* kCreateFoldersTable = R"(
CREATE TABLE IF NOT EXISTS folders (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  uuid TEXT NOT NULL UNIQUE,
  parent_id INTEGER,
  name TEXT NOT NULL,
  created_utc INTEGER NOT NULL,
  modified_utc INTEGER NOT NULL,
  metadata TEXT,
  FOREIGN KEY (parent_id) REFERENCES folders(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_folders_parent ON folders(parent_id);
CREATE INDEX IF NOT EXISTS idx_folders_uuid ON folders(uuid);
)";

// Files table: stores file metadata
// folder_id references folders(id)
// uuid is the string ID from JSON files (from FileRecord.id)
// metadata stores additional JSON data
inline constexpr const char* kCreateFilesTable = R"(
CREATE TABLE IF NOT EXISTS files (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  uuid TEXT NOT NULL UNIQUE,
  folder_id INTEGER NOT NULL,
  name TEXT NOT NULL,
  created_utc INTEGER NOT NULL,
  modified_utc INTEGER NOT NULL,
  metadata TEXT,
  FOREIGN KEY (folder_id) REFERENCES folders(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_files_folder ON files(folder_id);
CREATE INDEX IF NOT EXISTS idx_files_name ON files(name);
CREATE INDEX IF NOT EXISTS idx_files_uuid ON files(uuid);
)";

// Tags table: stores unique tags with hierarchy
// parent_id references tags(id) - NULL for root tags
// metadata stores additional JSON data
inline constexpr const char* kCreateTagsTable = R"(
CREATE TABLE IF NOT EXISTS tags (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL UNIQUE,
  parent_id INTEGER,
  metadata TEXT,
  FOREIGN KEY (parent_id) REFERENCES tags(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_tags_name ON tags(name);
CREATE INDEX IF NOT EXISTS idx_tags_parent ON tags(parent_id);
)";

// File-Tag many-to-many relationship
inline constexpr const char* kCreateFileTagsTable = R"(
CREATE TABLE IF NOT EXISTS file_tags (
  file_id INTEGER NOT NULL,
  tag_id INTEGER NOT NULL,
  PRIMARY KEY (file_id, tag_id),
  FOREIGN KEY (file_id) REFERENCES files(id) ON DELETE CASCADE,
  FOREIGN KEY (tag_id) REFERENCES tags(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_file_tags_tag ON file_tags(tag_id);
)";

// Notebook metadata: key-value store for notebook-level metadata
inline constexpr const char* kCreateNotebookMetadataTable = R"(
CREATE TABLE IF NOT EXISTS notebook_metadata (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
)";

// Schema version table for migrations
inline constexpr const char* kCreateSchemaVersionTable = R"(
CREATE TABLE IF NOT EXISTS schema_version (
  version INTEGER PRIMARY KEY
);
)";

// Table names in reverse dependency order (safe for dropping)
// When adding a new table, add it to this array in the appropriate position
inline constexpr const char* kTableNames[] = {
    "file_tags",          // Many-to-many relationship (depends on files, tags)
    "files",              // Depends on folders
    "tags",               // Independent
    "folders",            // Independent (now includes sync state)
    "notebook_metadata",  // Independent (key-value store)
    "schema_version",     // Independent
};

// Combined initialization script
inline const std::string GetInitializationScript() {
  return std::string(kCreateFoldersTable) + "\n" + std::string(kCreateFilesTable) + "\n" +
         std::string(kCreateTagsTable) + "\n" + std::string(kCreateFileTagsTable) + "\n" +
         std::string(kCreateNotebookMetadataTable) + "\n" + std::string(kCreateSchemaVersionTable);
}

// Generate DROP TABLE statements for all tables
inline const std::string GetDropAllTablesScript() {
  std::string script;
  for (const char* table : kTableNames) {
    script += "DROP TABLE IF EXISTS ";
    script += table;
    script += ";\n";
  }
  return script;
}

}  // namespace schema
}  // namespace db
}  // namespace vxcore

#endif  // VXCORE_DB_SCHEMA_H
