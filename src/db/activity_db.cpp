#include "activity_db.h"

#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include "utils/logger.h"

namespace vxcore {
namespace db {

namespace {

// Global per-day buckets. Week/month/year = SUM over a date range.
constexpr const char* kCreateActivityDailyTable = R"(
CREATE TABLE IF NOT EXISTS activity_daily (
  date          TEXT PRIMARY KEY,
  active_ms     INTEGER NOT NULL DEFAULT 0,
  notes_created INTEGER NOT NULL DEFAULT 0,
  notes_read    INTEGER NOT NULL DEFAULT 0,
  notes_edited  INTEGER NOT NULL DEFAULT 0
);
)";

// Per-file per-day counts. Hot files = SUM(reads+edits) over a window.
constexpr const char* kCreateFileActivityDailyTable = R"(
CREATE TABLE IF NOT EXISTS file_activity_daily (
  date        TEXT NOT NULL,
  notebook_id TEXT NOT NULL,
  file_id     TEXT NOT NULL,
  path        TEXT NOT NULL,
  reads       INTEGER NOT NULL DEFAULT 0,
  edits       INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (date, notebook_id, file_id)
);
CREATE INDEX IF NOT EXISTS idx_file_activity_file
  ON file_activity_daily (notebook_id, file_id);
)";

constexpr const char* kCreateSchemaVersionTable = R"(
CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);
)";

// Whitelist for IncrementDaily column names (never bind a column name).
bool IsValidDailyColumn(const std::string& column) {
  return column == "notes_created" || column == "notes_read" || column == "notes_edited";
}

}  // namespace

ActivityDb::ActivityDb(sqlite3* db) : db_(db) {}

std::string ActivityDb::GetLastError() const {
  if (db_ != nullptr) {
    return sqlite3_errmsg(db_);
  }
  return "Database not initialized";
}

bool ActivityDb::InitializeSchema() {
  if (!db_) {
    return false;
  }

  // activity.db is a best-effort, per-device cache. Under WAL, synchronous=
  // NORMAL is safe (no corruption on app crash, only a small window of the last
  // transactions may be lost on OS/power failure) and avoids an fsync per
  // commit -- important since flushes run on the UI thread.
  char* err_msg = nullptr;
  int rc = sqlite3_exec(db_, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    VXCORE_LOG_WARN("activity: failed to set synchronous=NORMAL: %s",
                    err_msg ? err_msg : GetLastError().c_str());
    if (err_msg) sqlite3_free(err_msg);
    err_msg = nullptr;
  }

  // Create the version table first so we can read/branch on it.
  rc = sqlite3_exec(db_, kCreateSchemaVersionTable, nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    VXCORE_LOG_ERROR("activity: failed to create schema_version: %s",
                     err_msg ? err_msg : GetLastError().c_str());
    if (err_msg) sqlite3_free(err_msg);
    return false;
  }

  int current = GetSchemaVersion();

  // Ordered, read-compare-upgrade migration path. Each numbered step upgrades
  // FROM (step-1) TO step. New future columns/tables append new steps here;
  // never rewrite an existing step (activity data is source-of-truth).
  //
  // Step 1: initial schema (activity_daily + file_activity_daily).
  if (current < 1) {
    rc = sqlite3_exec(db_, kCreateActivityDailyTable, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
      VXCORE_LOG_ERROR("activity: failed to create activity_daily: %s",
                       err_msg ? err_msg : GetLastError().c_str());
      if (err_msg) sqlite3_free(err_msg);
      return false;
    }
    rc = sqlite3_exec(db_, kCreateFileActivityDailyTable, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
      VXCORE_LOG_ERROR("activity: failed to create file_activity_daily: %s",
                       err_msg ? err_msg : GetLastError().c_str());
      if (err_msg) sqlite3_free(err_msg);
      return false;
    }
    current = 1;
  }

  // current now == kActivitySchemaVersion (add future upgrade blocks above).
  if (current != kActivitySchemaVersion) {
    VXCORE_LOG_ERROR("activity: unexpected schema version %d (expected %d)", current,
                     kActivitySchemaVersion);
    return false;
  }

  if (!SetSchemaVersion(kActivitySchemaVersion)) {
    return false;
  }

  VXCORE_LOG_DEBUG("activity: schema initialized (version %d)", kActivitySchemaVersion);
  return true;
}

int ActivityDb::GetSchemaVersion() {
  if (!db_) return 0;
  const char* sql = "SELECT version FROM schema_version LIMIT 1;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return 0;
  }
  int version = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    version = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return version;
}

bool ActivityDb::SetSchemaVersion(int version) {
  if (!db_) return false;
  // Single-row table: replace whatever is there.
  char* err_msg = nullptr;
  if (sqlite3_exec(db_, "DELETE FROM schema_version;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
    if (err_msg) sqlite3_free(err_msg);
    return false;
  }
  const char* sql = "INSERT INTO schema_version (version) VALUES (?);";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(stmt, 1, version);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool ActivityDb::AddFocusTime(const std::string& date, int64_t delta_ms) {
  if (!db_ || delta_ms <= 0) return false;
  const char* sql =
      "INSERT INTO activity_daily (date, active_ms) VALUES (?, ?) "
      "ON CONFLICT(date) DO UPDATE SET active_ms = active_ms + excluded.active_ms;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, date.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, delta_ms);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool ActivityDb::IncrementDaily(const std::string& date, const std::string& column, int64_t delta) {
  if (!db_ || !IsValidDailyColumn(column) || delta == 0) return false;
  // Column name is whitelisted above, so string-interpolating it is safe.
  std::string sql = "INSERT INTO activity_daily (date, " + column + ") VALUES (?, ?) " +
                    "ON CONFLICT(date) DO UPDATE SET " + column + " = " + column +
                    " + excluded." + column + ";";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, date.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, delta);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool ActivityDb::UpsertFileActivity(const std::string& date, const std::string& notebook_id,
                                    const std::string& file_id, const std::string& path,
                                    int64_t reads, int64_t edits) {
  if (!db_ || notebook_id.empty() || file_id.empty()) return false;
  const char* sql =
      "INSERT INTO file_activity_daily (date, notebook_id, file_id, path, reads, edits) "
      "VALUES (?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(date, notebook_id, file_id) DO UPDATE SET "
      "reads = reads + excluded.reads, "
      "edits = edits + excluded.edits, "
      "path = excluded.path;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, date.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, notebook_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, file_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, reads);
  sqlite3_bind_int64(stmt, 6, edits);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool ActivityDb::UpdateFilePath(const std::string& notebook_id, const std::string& file_id,
                                const std::string& path) {
  if (!db_ || notebook_id.empty() || file_id.empty()) return false;
  const char* sql =
      "UPDATE file_activity_daily SET path = ? WHERE notebook_id = ? AND file_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, notebook_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, file_id.c_str(), -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

std::string ActivityDb::GetRange(const std::string& from_date, const std::string& to_date) {
  nlohmann::json out;
  out["from"] = from_date;
  out["to"] = to_date;
  out["activeMs"] = 0;
  out["notesCreated"] = 0;
  out["notesRead"] = 0;
  out["notesEdited"] = 0;
  out["daily"] = nlohmann::json::array();
  if (!db_) return out.dump();

  const char* sql =
      "SELECT date, active_ms, notes_created, notes_read, notes_edited "
      "FROM activity_daily WHERE date >= ? AND date <= ? ORDER BY date ASC;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return out.dump();
  }
  sqlite3_bind_text(stmt, 1, from_date.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, to_date.c_str(), -1, SQLITE_TRANSIENT);

  int64_t sum_active = 0, sum_created = 0, sum_read = 0, sum_edited = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    nlohmann::json day;
    day["date"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    int64_t active = sqlite3_column_int64(stmt, 1);
    int64_t created = sqlite3_column_int64(stmt, 2);
    int64_t read = sqlite3_column_int64(stmt, 3);
    int64_t edited = sqlite3_column_int64(stmt, 4);
    day["activeMs"] = active;
    day["notesCreated"] = created;
    day["notesRead"] = read;
    day["notesEdited"] = edited;
    out["daily"].push_back(day);
    sum_active += active;
    sum_created += created;
    sum_read += read;
    sum_edited += edited;
  }
  sqlite3_finalize(stmt);

  out["activeMs"] = sum_active;
  out["notesCreated"] = sum_created;
  out["notesRead"] = sum_read;
  out["notesEdited"] = sum_edited;
  return out.dump();
}

std::string ActivityDb::GetHotFiles(const std::string& from_date, const std::string& to_date,
                                    int limit) {
  nlohmann::json out;
  out["files"] = nlohmann::json::array();
  if (!db_) return out.dump();
  if (limit <= 0) limit = 20;

  // Aggregate per file across the window; last-known path picked by most
  // recent date via MAX(date).
  const char* sql =
      "SELECT notebook_id, file_id, "
      "  (SELECT path FROM file_activity_daily f2 "
      "     WHERE f2.notebook_id = f.notebook_id AND f2.file_id = f.file_id "
      "     ORDER BY f2.date DESC LIMIT 1) AS last_path, "
      "  SUM(reads) AS r, SUM(edits) AS e "
      "FROM file_activity_daily f "
      "WHERE date >= ? AND date <= ? "
      "GROUP BY notebook_id, file_id "
      "ORDER BY (SUM(reads) + SUM(edits)) DESC, last_path ASC "
      "LIMIT ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return out.dump();
  }
  sqlite3_bind_text(stmt, 1, from_date.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, to_date.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, limit);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    nlohmann::json f;
    f["notebookId"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    f["fileId"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const unsigned char* path = sqlite3_column_text(stmt, 2);
    f["path"] = path ? reinterpret_cast<const char*>(path) : "";
    int64_t reads = sqlite3_column_int64(stmt, 3);
    int64_t edits = sqlite3_column_int64(stmt, 4);
    f["reads"] = reads;
    f["edits"] = edits;
    f["score"] = reads + edits;
    out["files"].push_back(f);
  }
  sqlite3_finalize(stmt);
  return out.dump();
}

std::string ActivityDb::GetFileHistory(const std::string& notebook_id,
                                       const std::string& file_id) {
  nlohmann::json out;
  out["notebookId"] = notebook_id;
  out["fileId"] = file_id;
  out["totalReads"] = 0;
  out["totalEdits"] = 0;
  out["daily"] = nlohmann::json::array();
  if (!db_) return out.dump();

  const char* sql =
      "SELECT date, reads, edits, path FROM file_activity_daily "
      "WHERE notebook_id = ? AND file_id = ? ORDER BY date ASC;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return out.dump();
  }
  sqlite3_bind_text(stmt, 1, notebook_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, file_id.c_str(), -1, SQLITE_TRANSIENT);

  int64_t total_reads = 0, total_edits = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    nlohmann::json day;
    day["date"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    int64_t reads = sqlite3_column_int64(stmt, 1);
    int64_t edits = sqlite3_column_int64(stmt, 2);
    day["reads"] = reads;
    day["edits"] = edits;
    const unsigned char* path = sqlite3_column_text(stmt, 3);
    day["path"] = path ? reinterpret_cast<const char*>(path) : "";
    out["daily"].push_back(day);
    total_reads += reads;
    total_edits += edits;
  }
  sqlite3_finalize(stmt);

  out["totalReads"] = total_reads;
  out["totalEdits"] = total_edits;
  return out.dump();
}

}  // namespace db
}  // namespace vxcore
