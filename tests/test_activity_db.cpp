// test_activity_db: DB-internal tests for ActivityDb (Pattern B).
//
// Direct-compiles db/activity_db.cpp + db/db_manager.cpp + needed utils; links
// sqlite3 + nlohmann_json. Exercises schema init/migration, UPSERT increments,
// range aggregation, hot-file ranking, retain-on-delete, and migration no-op.

#include <nlohmann/json.hpp>

#include <string>

#include "db/activity_db.h"
#include "db/db_manager.h"
#include "test_utils.h"

using vxcore::db::ActivityDb;
using vxcore::db::DbManager;

namespace {

std::string DbPath() { return get_test_path("activity_db/activity.db"); }

void FreshDir() {
  cleanup_test_dir(get_test_path("activity_db"));
  create_directory(get_test_path("activity_db"));
}

int test_schema_init_and_version() {
  std::cout << "  Running test_schema_init_and_version..." << std::endl;
  FreshDir();

  DbManager db;
  ASSERT_TRUE(db.Open(DbPath()));
  ActivityDb adb(db.GetHandle());
  ASSERT_TRUE(adb.InitializeSchema());
  ASSERT_EQ(adb.GetSchemaVersion(), ActivityDb::kActivitySchemaVersion);

  std::cout << "  ✓ passed" << std::endl;
  return 0;
}

int test_migration_noop_on_same_version() {
  std::cout << "  Running test_migration_noop_on_same_version..." << std::endl;
  FreshDir();

  {
    DbManager db;
    ASSERT_TRUE(db.Open(DbPath()));
    ActivityDb adb(db.GetHandle());
    ASSERT_TRUE(adb.InitializeSchema());
    // Seed a row so we can verify data survives a second init.
    ASSERT_TRUE(adb.IncrementDaily("2026-07-10", "notes_created", 3));
  }

  // Re-open and re-init: must be a no-op that preserves data.
  DbManager db;
  ASSERT_TRUE(db.Open(DbPath()));
  ActivityDb adb(db.GetHandle());
  ASSERT_TRUE(adb.InitializeSchema());
  ASSERT_EQ(adb.GetSchemaVersion(), ActivityDb::kActivitySchemaVersion);

  std::string json = adb.GetRange("2026-07-10", "2026-07-10");
  auto j = nlohmann::json::parse(json);
  ASSERT_EQ(j["notesCreated"].get<int>(), 3);

  std::cout << "  ✓ passed" << std::endl;
  return 0;
}

int test_daily_upsert_increments() {
  std::cout << "  Running test_daily_upsert_increments..." << std::endl;
  FreshDir();
  DbManager db;
  ASSERT_TRUE(db.Open(DbPath()));
  ActivityDb adb(db.GetHandle());
  ASSERT_TRUE(adb.InitializeSchema());

  ASSERT_TRUE(adb.IncrementDaily("2026-07-10", "notes_created", 1));
  ASSERT_TRUE(adb.IncrementDaily("2026-07-10", "notes_created", 1));
  ASSERT_TRUE(adb.IncrementDaily("2026-07-10", "notes_read", 5));
  ASSERT_TRUE(adb.IncrementDaily("2026-07-10", "notes_edited", 2));
  ASSERT_TRUE(adb.AddFocusTime("2026-07-10", 1000));
  ASSERT_TRUE(adb.AddFocusTime("2026-07-10", 500));

  // Invalid column rejected.
  ASSERT_FALSE(adb.IncrementDaily("2026-07-10", "bogus; DROP TABLE", 1));

  auto j = nlohmann::json::parse(adb.GetRange("2026-07-10", "2026-07-10"));
  ASSERT_EQ(j["notesCreated"].get<int>(), 2);
  ASSERT_EQ(j["notesRead"].get<int>(), 5);
  ASSERT_EQ(j["notesEdited"].get<int>(), 2);
  ASSERT_EQ(j["activeMs"].get<int>(), 1500);
  ASSERT_EQ(j["daily"].size(), 1u);

  std::cout << "  ✓ passed" << std::endl;
  return 0;
}

int test_range_aggregation() {
  std::cout << "  Running test_range_aggregation..." << std::endl;
  FreshDir();
  DbManager db;
  ASSERT_TRUE(db.Open(DbPath()));
  ActivityDb adb(db.GetHandle());
  ASSERT_TRUE(adb.InitializeSchema());

  adb.IncrementDaily("2026-07-08", "notes_read", 1);
  adb.IncrementDaily("2026-07-09", "notes_read", 2);
  adb.IncrementDaily("2026-07-10", "notes_read", 4);
  // Outside range.
  adb.IncrementDaily("2026-07-20", "notes_read", 100);

  auto j = nlohmann::json::parse(adb.GetRange("2026-07-08", "2026-07-10"));
  ASSERT_EQ(j["notesRead"].get<int>(), 7);
  ASSERT_EQ(j["daily"].size(), 3u);

  std::cout << "  ✓ passed" << std::endl;
  return 0;
}

int test_hot_files_ranking() {
  std::cout << "  Running test_hot_files_ranking..." << std::endl;
  FreshDir();
  DbManager db;
  ASSERT_TRUE(db.Open(DbPath()));
  ActivityDb adb(db.GetHandle());
  ASSERT_TRUE(adb.InitializeSchema());

  // file A: 2 reads + 1 edit = 3 (across two days)
  adb.UpsertFileActivity("2026-07-09", "nb1", "A", "a.md", 1, 0);
  adb.UpsertFileActivity("2026-07-10", "nb1", "A", "a.md", 1, 1);
  // file B: 10 reads = 10
  adb.UpsertFileActivity("2026-07-10", "nb1", "B", "b.md", 10, 0);
  // file C outside window
  adb.UpsertFileActivity("2026-07-01", "nb1", "C", "c.md", 50, 0);

  auto j = nlohmann::json::parse(adb.GetHotFiles("2026-07-08", "2026-07-11", 10));
  ASSERT_EQ(j["files"].size(), 2u);
  // B (score 10) ranks first, then A (score 3).
  ASSERT_EQ(j["files"][0]["fileId"].get<std::string>(), std::string("B"));
  ASSERT_EQ(j["files"][0]["score"].get<int>(), 10);
  ASSERT_EQ(j["files"][1]["fileId"].get<std::string>(), std::string("A"));
  ASSERT_EQ(j["files"][1]["score"].get<int>(), 3);

  // Limit honored.
  auto j1 = nlohmann::json::parse(adb.GetHotFiles("2026-07-08", "2026-07-11", 1));
  ASSERT_EQ(j1["files"].size(), 1u);

  std::cout << "  ✓ passed" << std::endl;
  return 0;
}

int test_file_history_and_retain_on_delete() {
  std::cout << "  Running test_file_history_and_retain_on_delete..." << std::endl;
  FreshDir();
  DbManager db;
  ASSERT_TRUE(db.Open(DbPath()));
  ActivityDb adb(db.GetHandle());
  ASSERT_TRUE(adb.InitializeSchema());

  adb.UpsertFileActivity("2026-07-09", "nb1", "A", "old/a.md", 1, 0);
  adb.UpsertFileActivity("2026-07-10", "nb1", "A", "old/a.md", 0, 2);
  // Simulate a rename/move: path updates but rows retained.
  ASSERT_TRUE(adb.UpdateFilePath("nb1", "A", "new/a.md"));

  auto j = nlohmann::json::parse(adb.GetFileHistory("nb1", "A"));
  ASSERT_EQ(j["totalReads"].get<int>(), 1);
  ASSERT_EQ(j["totalEdits"].get<int>(), 2);
  ASSERT_EQ(j["daily"].size(), 2u);
  // Path was refreshed on all rows (retain-on-delete keeps last-known path).
  ASSERT_EQ(j["daily"][0]["path"].get<std::string>(), std::string("new/a.md"));

  std::cout << "  ✓ passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  RUN_TEST(test_schema_init_and_version);
  RUN_TEST(test_migration_noop_on_same_version);
  RUN_TEST(test_daily_upsert_increments);
  RUN_TEST(test_range_aggregation);
  RUN_TEST(test_hot_files_ranking);
  RUN_TEST(test_file_history_and_retain_on_delete);
  std::cout << "All test_activity_db tests passed" << std::endl;
  return 0;
}
