#include "db_manager.h"

#include <sqlite3.h>

#include <optional>

#include "db_schema.h"
#include "utils/logger.h"

namespace vxcore {
namespace db {

DbManager::DbManager() : db_(nullptr), db_path_() {}

DbManager::~DbManager() { Close(); }

bool DbManager::Open(const std::string& db_path) {
  if (db_ != nullptr) {
    VXCORE_LOG_WARN("Database already open, closing previous connection");
    Close();
  }

  VXCORE_LOG_DEBUG("Opening database: %s", db_path.c_str());

  int rc = sqlite3_open(db_path.c_str(), &db_);
  if (rc != SQLITE_OK) {
    std::string error = GetLastError();
    VXCORE_LOG_ERROR("Failed to open database '%s': %s", db_path.c_str(), error.c_str());
    Close();
    return false;
  }

  db_path_ = db_path;

  // Enable foreign keys
  char* err_msg = nullptr;
  rc = sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    std::string error = err_msg ? err_msg : GetLastError();
    VXCORE_LOG_ERROR("Failed to enable foreign keys: %s", error.c_str());
    if (err_msg) {
      sqlite3_free(err_msg);
    }
    Close();
    return false;
  }

  // Enable WAL mode for better concurrency
  rc = sqlite3_exec(db_, "PRAGMA journal_mode = WAL;", nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    std::string error = err_msg ? err_msg : GetLastError();
    VXCORE_LOG_ERROR("Failed to enable WAL mode: %s", error.c_str());
    if (err_msg) {
      sqlite3_free(err_msg);
    }
    Close();
    return false;
  }

  VXCORE_LOG_INFO("Database opened successfully: %s", db_path.c_str());
  return true;
}

void DbManager::Close() {
  if (db_ != nullptr) {
    VXCORE_LOG_DEBUG("Closing database: %s", db_path_.c_str());
    int rc = sqlite3_close_v2(db_);
    if (rc != SQLITE_OK) {
      VXCORE_LOG_WARN("Error closing database: %s", GetLastError().c_str());
    }
    db_ = nullptr;
    db_path_.clear();
  }
}

bool DbManager::IsOpen() const { return db_ != nullptr; }

std::string DbManager::GetPath() const { return db_path_; }

bool DbManager::InitializeSchema() {
  if (!IsOpen()) {
    VXCORE_LOG_ERROR("Cannot initialize schema: database not open");
    return false;
  }

  VXCORE_LOG_DEBUG("Initializing database schema");

  std::string init_script = schema::GetInitializationScript();

  char* err_msg = nullptr;
  int rc = sqlite3_exec(db_, init_script.c_str(), nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    std::string error = err_msg ? err_msg : GetLastError();
    VXCORE_LOG_ERROR("Failed to execute schema initialization: %s", error.c_str());
    if (err_msg) {
      sqlite3_free(err_msg);
    }
    return false;
  }

  // Insert or update schema version
  const char* version_sql = "INSERT OR REPLACE INTO schema_version (version) VALUES (?);";
  sqlite3_stmt* stmt = nullptr;
  rc = sqlite3_prepare_v2(db_, version_sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    VXCORE_LOG_ERROR("Failed to prepare schema version statement: %s", GetLastError().c_str());
    return false;
  }

  sqlite3_bind_int(stmt, 1, schema::kCurrentSchemaVersion);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    VXCORE_LOG_ERROR("Failed to set schema version: %s", GetLastError().c_str());
    return false;
  }

  VXCORE_LOG_INFO("Database schema initialized (version %d)", schema::kCurrentSchemaVersion);
  return true;
}

bool DbManager::RebuildDatabase() {
  if (!IsOpen()) {
    VXCORE_LOG_ERROR("Cannot rebuild database: database not open");
    return false;
  }

  VXCORE_LOG_INFO("Rebuilding database: %s", db_path_.c_str());

  // Drop all tables using centralized schema definition
  std::string drop_script = schema::GetDropAllTablesScript();

  char* err_msg = nullptr;
  int rc = sqlite3_exec(db_, drop_script.c_str(), nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    std::string error = err_msg ? err_msg : GetLastError();
    VXCORE_LOG_ERROR("Failed to drop tables: %s", error.c_str());
    if (err_msg) {
      sqlite3_free(err_msg);
    }
    return false;
  }

  // Recreate schema
  bool success = InitializeSchema();
  if (success) {
    VXCORE_LOG_INFO("Database rebuilt successfully");
  }
  return success;
}

bool DbManager::BeginTransaction() {
  if (!IsOpen()) {
    VXCORE_LOG_ERROR("Cannot begin transaction: database not open");
    return false;
  }

  char* err_msg = nullptr;
  int rc = sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    std::string error = err_msg ? err_msg : GetLastError();
    VXCORE_LOG_ERROR("Failed to begin transaction: %s", error.c_str());
    if (err_msg) {
      sqlite3_free(err_msg);
    }
    return false;
  }
  return true;
}

bool DbManager::CommitTransaction() {
  if (!IsOpen()) {
    VXCORE_LOG_ERROR("Cannot commit transaction: database not open");
    return false;
  }

  char* err_msg = nullptr;
  int rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    std::string error = err_msg ? err_msg : GetLastError();
    VXCORE_LOG_ERROR("Failed to commit transaction: %s", error.c_str());
    if (err_msg) {
      sqlite3_free(err_msg);
    }
    return false;
  }
  return true;
}

bool DbManager::RollbackTransaction() {
  if (!IsOpen()) {
    VXCORE_LOG_ERROR("Cannot rollback transaction: database not open");
    return false;
  }

  char* err_msg = nullptr;
  int rc = sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    std::string error = err_msg ? err_msg : GetLastError();
    VXCORE_LOG_ERROR("Failed to rollback transaction: %s", error.c_str());
    if (err_msg) {
      sqlite3_free(err_msg);
    }
    return false;
  }
  return true;
}

std::string DbManager::GetLastError() const {
  if (db_ != nullptr) {
    return sqlite3_errmsg(db_);
  }
  return "Database not open";
}

}  // namespace db
}  // namespace vxcore
