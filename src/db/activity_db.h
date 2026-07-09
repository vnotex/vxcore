#ifndef VXCORE_ACTIVITY_DB_H
#define VXCORE_ACTIVITY_DB_H

#include <cstdint>
#include <string>

// Forward declare sqlite3 to avoid exposing SQLite types in header
struct sqlite3;

namespace vxcore {
namespace db {

// Activity database operations over a shared sqlite3 handle.
//
// Owns NO connection lifecycle (the caller's DbManager owns the sqlite3*).
// This class implements the schema, a real read-compare-upgrade migration
// path, cheap single-row UPSERTs, and read-side aggregation queries for the
// activity-tracking feature.
//
// Storage model: global per-day buckets (`activity_daily`) plus per-file
// per-day counts (`file_activity_daily`). Week/month/year totals are computed
// on read by summing over a date range. All dates are local calendar dates
// formatted 'YYYY-MM-DD'.
//
// NOT thread-safe: caller must ensure single-threaded access.
class ActivityDb {
 public:
  explicit ActivityDb(sqlite3* db);
  ~ActivityDb() = default;

  // Disable copy/move
  ActivityDb(const ActivityDb&) = delete;
  ActivityDb& operator=(const ActivityDb&) = delete;

  // The schema version this build expects. Bump when adding migration steps.
  static constexpr int kActivitySchemaVersion = 1;

  // Creates tables if missing and runs the ordered migration path from the
  // on-disk schema_version up to kActivitySchemaVersion. Idempotent.
  // Returns true on success.
  bool InitializeSchema();

  // Returns the persisted schema version, or 0 if the table/row is absent.
  int GetSchemaVersion();

  // --- Recording (all single-row UPSERTs, cheap) ---

  // Adds focused milliseconds to the given local date.
  bool AddFocusTime(const std::string& date, int64_t delta_ms);

  // Increments a global daily counter. |column| must be one of
  // "notes_created", "notes_read", "notes_edited".
  bool IncrementDaily(const std::string& date, const std::string& column, int64_t delta);

  // Upserts a per-file per-day row, adding |reads| and |edits| and refreshing
  // the last-known display path. Rows are retained on delete so history
  // survives.
  bool UpsertFileActivity(const std::string& date, const std::string& notebook_id,
                          const std::string& file_id, const std::string& path, int64_t reads,
                          int64_t edits);

  // Updates only the last-known path for all rows of a file (e.g. on rename).
  bool UpdateFilePath(const std::string& notebook_id, const std::string& file_id,
                      const std::string& path);

  // --- Queries (return JSON strings) ---

  // Summed totals over [from_date, to_date] inclusive, plus a per-day series.
  // {"from","to","activeMs","notesCreated","notesRead","notesEdited",
  //  "daily":[{"date","activeMs","notesCreated","notesRead","notesEdited"}]}
  std::string GetRange(const std::string& from_date, const std::string& to_date);

  // Top files by (reads + edits) over [from_date, to_date], limited to |limit|.
  // {"files":[{"notebookId","fileId","path","reads","edits","score"}]}
  std::string GetHotFiles(const std::string& from_date, const std::string& to_date, int limit);

  // Full per-day history for one file across all dates.
  // {"notebookId","fileId","totalReads","totalEdits",
  //  "daily":[{"date","reads","edits","path"}]}
  std::string GetFileHistory(const std::string& notebook_id, const std::string& file_id);

  // Returns the last SQLite error message.
  std::string GetLastError() const;

 private:
  // Sets the persisted schema version (INSERT OR REPLACE single row).
  bool SetSchemaVersion(int version);

  sqlite3* db_;
};

}  // namespace db
}  // namespace vxcore

#endif  // VXCORE_ACTIVITY_DB_H
