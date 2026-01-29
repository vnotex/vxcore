#ifndef VXCORE_TAG_DB_H
#define VXCORE_TAG_DB_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Forward declare sqlite3
struct sqlite3;

namespace vxcore {
namespace db {

// Tag metadata structure
struct TagRecord {
  int64_t id;
  std::string name;
  int64_t parent_id;     // -1 for root tags
  std::string metadata;  // JSON string
};

// Tag query result
struct TagQueryResult {
  int64_t file_id;
  int64_t folder_id;
  std::string file_name;
  std::vector<std::string> tags;
};

// Tag database operations (CRUD + queries)
// NOT thread-safe: caller must ensure synchronization
class TagDb {
 public:
  explicit TagDb(sqlite3* db);
  ~TagDb() = default;

  // Disable copy/move
  TagDb(const TagDb&) = delete;
  TagDb& operator=(const TagDb&) = delete;

  // --- Tag CRUD Operations ---

  // Creates or updates a tag with hierarchy, returns tag_id or -1 on error
  // parent_id = -1 for root tags
  // metadata is stored as JSON string
  int64_t CreateOrUpdateTag(const std::string& tag_name, int64_t parent_id,
                            const std::string& metadata);

  // Gets or creates a tag by name only, returns tag_id or -1 on error
  // Uses parent_id = -1 (root) and empty metadata
  int64_t GetOrCreateTag(const std::string& tag_name);

  // Gets a tag by name, returns nullopt if not found
  std::optional<TagRecord> GetTag(const std::string& tag_name);

  // Gets a tag by id, returns nullopt if not found
  std::optional<TagRecord> GetTagById(int64_t tag_id);

  // Deletes a tag (cascade will delete children and file associations)
  bool DeleteTag(int64_t tag_id);

  // Lists all tags (for notebook config synchronization)
  std::vector<TagRecord> ListAllTags();

  // Lists child tags of a parent tag
  // parent_id = -1 for root tags
  std::vector<TagRecord> ListChildTags(int64_t parent_id);

  // --- Tag Query Operations ---

  // Finds files that have ALL of the given tags (AND logic)
  std::vector<TagQueryResult> FindFilesByTagsAnd(const std::vector<std::string>& tags);

  // Finds files that have ANY of the given tags (OR logic)
  std::vector<TagQueryResult> FindFilesByTagsOr(const std::vector<std::string>& tags);

  // Counts files for each tag
  // Returns vector of pairs: (tag_name, file_count)
  std::vector<std::pair<std::string, int>> CountFilesByTag();

  // Returns the last error message
  std::string GetLastError() const;

 private:
  sqlite3* db_;
};

}  // namespace db
}  // namespace vxcore

#endif  // VXCORE_TAG_DB_H
