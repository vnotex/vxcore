#ifndef VXCORE_NOTEBOOK_DB_H
#define VXCORE_NOTEBOOK_DB_H

#include <optional>
#include <string>

// Forward declare sqlite3
struct sqlite3;

namespace vxcore {
namespace db {

// Notebook database operations (notebook-level metadata key-value store)
// NOT thread-safe: caller must ensure synchronization
class NotebookDb {
 public:
  explicit NotebookDb(sqlite3* db);
  ~NotebookDb() = default;

  // Disable copy/move
  NotebookDb(const NotebookDb&) = delete;
  NotebookDb& operator=(const NotebookDb&) = delete;

  // --- Notebook Metadata Operations ---

  // Gets a metadata value by key, returns nullopt if not found
  std::optional<std::string> GetMetadata(const std::string& key);

  // Sets a metadata value, returns true on success
  // Uses INSERT OR REPLACE semantics (creates or updates)
  bool SetMetadata(const std::string& key, const std::string& value);

  // Deletes a metadata entry, returns true on success
  bool DeleteMetadata(const std::string& key);

  // Returns the last error message
  std::string GetLastError() const;

 private:
  sqlite3* db_;
};

}  // namespace db
}  // namespace vxcore

#endif  // VXCORE_NOTEBOOK_DB_H
