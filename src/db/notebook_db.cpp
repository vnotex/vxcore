#include "notebook_db.h"

#include <sqlite3.h>

#include "utils/logger.h"

namespace vxcore {
namespace db {

NotebookDb::NotebookDb(sqlite3* db) : db_(db) {}

std::optional<std::string> NotebookDb::GetMetadata(const std::string& key) {
  if (!db_) {
    VXCORE_LOG_ERROR("Cannot get notebook metadata: database not open");
    return std::nullopt;
  }

  const char* sql = "SELECT value FROM notebook_metadata WHERE key = ?;";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    VXCORE_LOG_ERROR("Failed to prepare get metadata statement: %s", GetLastError().c_str());
    return std::nullopt;
  }

  sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);

  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  std::string value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return value;
}

bool NotebookDb::SetMetadata(const std::string& key, const std::string& value) {
  if (!db_) {
    VXCORE_LOG_ERROR("Cannot set notebook metadata: database not open");
    return false;
  }

  const char* sql = "INSERT OR REPLACE INTO notebook_metadata (key, value) VALUES (?, ?);";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    VXCORE_LOG_ERROR("Failed to prepare set metadata statement: %s", GetLastError().c_str());
    return false;
  }

  sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    VXCORE_LOG_ERROR("Failed to set notebook metadata: %s", GetLastError().c_str());
    return false;
  }

  return true;
}

bool NotebookDb::DeleteMetadata(const std::string& key) {
  if (!db_) {
    VXCORE_LOG_ERROR("Cannot delete notebook metadata: database not open");
    return false;
  }

  const char* sql = "DELETE FROM notebook_metadata WHERE key = ?;";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    VXCORE_LOG_ERROR("Failed to prepare delete metadata statement: %s", GetLastError().c_str());
    return false;
  }

  sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    VXCORE_LOG_ERROR("Failed to delete notebook metadata: %s", GetLastError().c_str());
    return false;
  }

  return true;
}

std::string NotebookDb::GetLastError() const {
  if (db_ != nullptr) {
    return sqlite3_errmsg(db_);
  }
  return "Database not open";
}

}  // namespace db
}  // namespace vxcore
