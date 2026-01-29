#ifndef VXCORE_DB_MANAGER_H
#define VXCORE_DB_MANAGER_H

#include <memory>
#include <string>

// Forward declare sqlite3 to avoid exposing SQLite types in header
struct sqlite3;

namespace vxcore {
namespace db {

// Database manager handles SQLite database lifecycle and schema management
class DbManager {
 public:
  DbManager();
  ~DbManager();

  // Disable copy/move
  DbManager(const DbManager&) = delete;
  DbManager& operator=(const DbManager&) = delete;
  DbManager(DbManager&&) = delete;
  DbManager& operator=(DbManager&&) = delete;

  // Opens or creates database at given path
  // Returns true on success, false on failure
  bool Open(const std::string& db_path);

  // Closes the database connection
  void Close();

  // Returns true if database is currently open
  bool IsOpen() const;

  // Returns the database file path (empty if not open)
  std::string GetPath() const;

  // Initializes database schema (creates tables if they don't exist)
  // Returns true on success, false on failure
  bool InitializeSchema();

  // Rebuilds entire database by dropping and recreating all tables
  // WARNING: This deletes all data!
  // Returns true on success, false on failure
  bool RebuildDatabase();

  // Transaction management
  bool BeginTransaction();
  bool CommitTransaction();
  bool RollbackTransaction();

  // Direct access to sqlite3 handle (for use by FileDb, TagDb, etc.)
  // Returns nullptr if database is not open
  sqlite3* GetHandle() const { return db_; }

  // Returns the last SQLite error message
  std::string GetLastError() const;

 private:
  sqlite3* db_;
  std::string db_path_;
};

}  // namespace db
}  // namespace vxcore

#endif  // VXCORE_DB_MANAGER_H
