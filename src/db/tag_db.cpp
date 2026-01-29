#include "tag_db.h"

#include <sqlite3.h>

namespace vxcore {
namespace db {

TagDb::TagDb(sqlite3* db) : db_(db) {}

std::string TagDb::GetLastError() const {
  if (db_ != nullptr) {
    return sqlite3_errmsg(db_);
  }
  return "Database not initialized";
}

std::vector<TagQueryResult> TagDb::FindFilesByTagsAnd(const std::vector<std::string>& tags) {
  if (tags.empty()) {
    return {};
  }

  // Build query: find files that have ALL tags
  // Strategy: Join file_tags once per tag, ensuring all match
  std::string sql =
      "SELECT DISTINCT f.id, f.folder_id, f.name "
      "FROM files f ";

  for (size_t i = 0; i < tags.size(); ++i) {
    sql += "JOIN file_tags ft" + std::to_string(i) + " ON f.id = ft" + std::to_string(i) +
           ".file_id "
           "JOIN tags t" +
           std::to_string(i) + " ON ft" + std::to_string(i) + ".tag_id = t" + std::to_string(i) +
           ".id AND t" + std::to_string(i) + ".name = ? ";
  }

  sql += "ORDER BY f.name;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return {};
  }

  // Bind tag names
  for (size_t i = 0; i < tags.size(); ++i) {
    sqlite3_bind_text(stmt, static_cast<int>(i + 1), tags[i].c_str(), -1, SQLITE_TRANSIENT);
  }

  std::vector<TagQueryResult> results;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    TagQueryResult result;
    result.file_id = sqlite3_column_int64(stmt, 0);
    result.folder_id = sqlite3_column_int64(stmt, 1);
    result.file_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

    // Get all tags for this file
    const char* tag_sql =
        "SELECT t.name FROM tags t "
        "JOIN file_tags ft ON t.id = ft.tag_id "
        "WHERE ft.file_id = ? ORDER BY t.name;";

    sqlite3_stmt* tag_stmt = nullptr;
    sqlite3_prepare_v2(db_, tag_sql, -1, &tag_stmt, nullptr);
    sqlite3_bind_int64(tag_stmt, 1, result.file_id);

    while (sqlite3_step(tag_stmt) == SQLITE_ROW) {
      result.tags.push_back(reinterpret_cast<const char*>(sqlite3_column_text(tag_stmt, 0)));
    }
    sqlite3_finalize(tag_stmt);

    results.push_back(result);
  }

  sqlite3_finalize(stmt);
  return results;
}

std::vector<TagQueryResult> TagDb::FindFilesByTagsOr(const std::vector<std::string>& tags) {
  if (tags.empty()) {
    return {};
  }

  // Build query: find files that have ANY of the tags
  std::string sql =
      "SELECT DISTINCT f.id, f.folder_id, f.name "
      "FROM files f "
      "JOIN file_tags ft ON f.id = ft.file_id "
      "JOIN tags t ON ft.tag_id = t.id "
      "WHERE t.name IN (";

  for (size_t i = 0; i < tags.size(); ++i) {
    if (i > 0) sql += ", ";
    sql += "?";
  }
  sql += ") ORDER BY f.name;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return {};
  }

  // Bind tag names
  for (size_t i = 0; i < tags.size(); ++i) {
    sqlite3_bind_text(stmt, static_cast<int>(i + 1), tags[i].c_str(), -1, SQLITE_TRANSIENT);
  }

  std::vector<TagQueryResult> results;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    TagQueryResult result;
    result.file_id = sqlite3_column_int64(stmt, 0);
    result.folder_id = sqlite3_column_int64(stmt, 1);
    result.file_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

    // Get all tags for this file
    const char* tag_sql =
        "SELECT t.name FROM tags t "
        "JOIN file_tags ft ON t.id = ft.tag_id "
        "WHERE ft.file_id = ? ORDER BY t.name;";

    sqlite3_stmt* tag_stmt = nullptr;
    sqlite3_prepare_v2(db_, tag_sql, -1, &tag_stmt, nullptr);
    sqlite3_bind_int64(tag_stmt, 1, result.file_id);

    while (sqlite3_step(tag_stmt) == SQLITE_ROW) {
      result.tags.push_back(reinterpret_cast<const char*>(sqlite3_column_text(tag_stmt, 0)));
    }
    sqlite3_finalize(tag_stmt);

    results.push_back(result);
  }

  sqlite3_finalize(stmt);
  return results;
}

std::vector<std::pair<std::string, int>> TagDb::CountFilesByTag() {
  const char* sql =
      "SELECT t.name, COUNT(ft.file_id) as file_count "
      "FROM tags t "
      "LEFT JOIN file_tags ft ON t.id = ft.tag_id "
      "GROUP BY t.id, t.name "
      "ORDER BY file_count DESC, t.name;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return {};
  }

  std::vector<std::pair<std::string, int>> results;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    std::string tag_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    int count = sqlite3_column_int(stmt, 1);
    results.push_back({tag_name, count});
  }

  sqlite3_finalize(stmt);
  return results;
}

// --- Tag CRUD Operations ---

int64_t TagDb::CreateOrUpdateTag(const std::string& tag_name, int64_t parent_id,
                                 const std::string& metadata) {
  // Check if tag exists
  auto existing = GetTag(tag_name);
  if (existing) {
    // Update existing tag
    const char* update_sql = "UPDATE tags SET parent_id = ?, metadata = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, update_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
      return -1;
    }

    if (parent_id == -1) {
      sqlite3_bind_null(stmt, 1);
    } else {
      sqlite3_bind_int64(stmt, 1, parent_id);
    }
    sqlite3_bind_text(stmt, 2, metadata.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, existing->id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? existing->id : -1;
  }

  // Create new tag
  const char* insert_sql = "INSERT INTO tags (name, parent_id, metadata) VALUES (?, ?, ?);";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return -1;
  }

  sqlite3_bind_text(stmt, 1, tag_name.c_str(), -1, SQLITE_TRANSIENT);
  if (parent_id == -1) {
    sqlite3_bind_null(stmt, 2);
  } else {
    sqlite3_bind_int64(stmt, 2, parent_id);
  }
  sqlite3_bind_text(stmt, 3, metadata.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  int64_t tag_id = -1;
  if (rc == SQLITE_DONE) {
    tag_id = sqlite3_last_insert_rowid(db_);
  }

  sqlite3_finalize(stmt);
  return tag_id;
}

int64_t TagDb::GetOrCreateTag(const std::string& tag_name) {
  // Try to get existing tag
  auto existing = GetTag(tag_name);
  if (existing) {
    return existing->id;
  }

  // Create new tag with parent_id = NULL and empty metadata
  return CreateOrUpdateTag(tag_name, -1, "");
}

std::optional<TagRecord> TagDb::GetTag(const std::string& tag_name) {
  const char* sql = "SELECT id, name, parent_id, metadata FROM tags WHERE name = ?;";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return std::nullopt;
  }

  sqlite3_bind_text(stmt, 1, tag_name.c_str(), -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);

  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  TagRecord tag;
  tag.id = sqlite3_column_int64(stmt, 0);
  tag.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  tag.parent_id = sqlite3_column_type(stmt, 2) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 2);
  tag.metadata = sqlite3_column_type(stmt, 3) == SQLITE_NULL
                     ? ""
                     : reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

  sqlite3_finalize(stmt);
  return tag;
}

std::optional<TagRecord> TagDb::GetTagById(int64_t tag_id) {
  const char* sql = "SELECT id, name, parent_id, metadata FROM tags WHERE id = ?;";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return std::nullopt;
  }

  sqlite3_bind_int64(stmt, 1, tag_id);
  rc = sqlite3_step(stmt);

  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  TagRecord tag;
  tag.id = sqlite3_column_int64(stmt, 0);
  tag.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  tag.parent_id = sqlite3_column_type(stmt, 2) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 2);
  tag.metadata = sqlite3_column_type(stmt, 3) == SQLITE_NULL
                     ? ""
                     : reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

  sqlite3_finalize(stmt);
  return tag;
}

bool TagDb::DeleteTag(int64_t tag_id) {
  // Foreign key cascade will handle deletion of children and file associations
  const char* sql = "DELETE FROM tags WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return false;
  }

  sqlite3_bind_int64(stmt, 1, tag_id);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

std::vector<TagRecord> TagDb::ListAllTags() {
  const char* sql = "SELECT id, name, parent_id, metadata FROM tags ORDER BY name;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return {};
  }

  std::vector<TagRecord> tags;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    TagRecord tag;
    tag.id = sqlite3_column_int64(stmt, 0);
    tag.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    tag.parent_id =
        sqlite3_column_type(stmt, 2) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 2);
    tag.metadata = sqlite3_column_type(stmt, 3) == SQLITE_NULL
                       ? ""
                       : reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    tags.push_back(tag);
  }

  sqlite3_finalize(stmt);
  return tags;
}

std::vector<TagRecord> TagDb::ListChildTags(int64_t parent_id) {
  const char* sql;
  if (parent_id == -1) {
    sql =
        "SELECT id, name, parent_id, metadata FROM tags "
        "WHERE parent_id IS NULL ORDER BY name;";
  } else {
    sql =
        "SELECT id, name, parent_id, metadata FROM tags "
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

  std::vector<TagRecord> tags;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    TagRecord tag;
    tag.id = sqlite3_column_int64(stmt, 0);
    tag.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    tag.parent_id =
        sqlite3_column_type(stmt, 2) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 2);
    tag.metadata = sqlite3_column_type(stmt, 3) == SQLITE_NULL
                       ? ""
                       : reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    tags.push_back(tag);
  }

  sqlite3_finalize(stmt);
  return tags;
}

}  // namespace db
}  // namespace vxcore
