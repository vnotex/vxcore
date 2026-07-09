// test_activity: C-API tests for vxcore_activity_* (Pattern A, links vxcore.dll).
//
// Calls vxcore_set_test_mode(1) first to redirect data paths to %TEMP%. Creates
// a notebook, records reads/edits/focus, drives file events via file create +
// buffer save, then queries JSON and verifies totals + free semantics.

#include <nlohmann/json.hpp>

#include <ctime>
#include <string>

#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace {

std::string TodayLocal() {
  std::time_t t = std::time(nullptr);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
  return std::string(buf);
}

int test_focus_and_record_queries() {
  std::cout << "  Running test_focus_and_record_queries..." << std::endl;
  const std::string root = get_test_path("activity_capi");
  cleanup_test_dir(root);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  ASSERT_NOT_NULL(ctx);

  char *nb_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, root.c_str(), "{\"name\":\"Act\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &nb_id),
            VXCORE_OK);
  ASSERT_NOT_NULL(nb_id);

  const std::string today = TodayLocal();

  // activity.db is app-global and persists across runs/tests; assert on DELTAS
  // from a baseline rather than absolute counts.
  auto read_totals = [&](int &active, int &read, int &edited) {
    char *j = nullptr;
    ASSERT_EQ(vxcore_activity_get_range(ctx, today.c_str(), today.c_str(), &j), VXCORE_OK);
    auto parsed = nlohmann::json::parse(j);
    active = parsed["activeMs"].get<int>();
    read = parsed["notesRead"].get<int>();
    edited = parsed["notesEdited"].get<int>();
    vxcore_string_free(j);
    return 0;
  };

  int base_active = 0, base_read = 0, base_edited = 0;
  if (read_totals(base_active, base_read, base_edited) != 0) return 1;

  // Focus time accumulates.
  ASSERT_EQ(vxcore_activity_add_focus_time(ctx, 1000), VXCORE_OK);
  ASSERT_EQ(vxcore_activity_add_focus_time(ctx, 500), VXCORE_OK);
  // Non-positive is a no-op success.
  ASSERT_EQ(vxcore_activity_add_focus_time(ctx, 0), VXCORE_OK);

  // Record a read (path may not resolve to a file id, but global count bumps).
  ASSERT_EQ(vxcore_activity_record_read(ctx, nb_id, "note.md"), VXCORE_OK);
  ASSERT_EQ(vxcore_activity_record_read(ctx, nb_id, "note.md"), VXCORE_OK);
  ASSERT_EQ(vxcore_activity_record_edit(ctx, nb_id, "note.md"), VXCORE_OK);

  int active = 0, read = 0, edited = 0;
  if (read_totals(active, read, edited) != 0) return 1;
  ASSERT_EQ(active - base_active, 1500);
  ASSERT_EQ(read - base_read, 2);
  ASSERT_EQ(edited - base_edited, 1);

  const std::string t = today;  // alias for query calls below
  char *hot_json = nullptr;
  ASSERT_EQ(vxcore_activity_get_hot_files(ctx, t.c_str(), t.c_str(), 5, &hot_json), VXCORE_OK);
  ASSERT_NOT_NULL(hot_json);
  auto jh = nlohmann::json::parse(hot_json);
  ASSERT_TRUE(jh.contains("files"));
  vxcore_string_free(hot_json);

  char *hist_json = nullptr;
  ASSERT_EQ(vxcore_activity_get_file_history(ctx, nb_id, "some-id", &hist_json), VXCORE_OK);
  ASSERT_NOT_NULL(hist_json);
  vxcore_string_free(hist_json);

  // Null-pointer guards.
  ASSERT_EQ(vxcore_activity_add_focus_time(nullptr, 1), VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_activity_record_read(ctx, nullptr, "x"), VXCORE_ERR_NULL_POINTER);

  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(root);
  std::cout << "  ✓ passed" << std::endl;
  return 0;
}

int test_created_and_saved_events_counted() {
  std::cout << "  Running test_created_and_saved_events_counted..." << std::endl;
  const std::string root = get_test_path("activity_events");
  cleanup_test_dir(root);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *nb_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, root.c_str(), "{\"name\":\"Ev\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &nb_id),
            VXCORE_OK);

  // Creating a file emits file.created -> notes_created += 1.
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, nb_id, "", "note.md", &file_id), VXCORE_OK);

  // Open, modify, and save the buffer. The save emits file.saved, which must
  // record BOTH the global notes_edited counter AND a per-file edit row (the
  // per-file id is resolved at flush time on the owner thread).
  char *buf_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, nb_id, "note.md", &buf_id), VXCORE_OK);
  ASSERT_NOT_NULL(buf_id);
  const char *content = "# hello\n";
  ASSERT_EQ(vxcore_buffer_set_content_raw(ctx, buf_id, content, 8), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(ctx, buf_id), VXCORE_OK);

  const std::string today = TodayLocal();
  char *range_json = nullptr;
  ASSERT_EQ(vxcore_activity_get_range(ctx, today.c_str(), today.c_str(), &range_json), VXCORE_OK);
  auto jr = nlohmann::json::parse(range_json);
  ASSERT_TRUE(jr["notesCreated"].get<int>() >= 1);
  ASSERT_TRUE(jr["notesEdited"].get<int>() >= 1);
  vxcore_string_free(range_json);

  // Per-file edit row must exist for the saved file (finding #1 regression).
  char *hist_json = nullptr;
  ASSERT_EQ(vxcore_activity_get_file_history(ctx, nb_id, file_id, &hist_json), VXCORE_OK);
  auto jhist = nlohmann::json::parse(hist_json);
  ASSERT_TRUE(jhist["totalEdits"].get<int>() >= 1);
  vxcore_string_free(hist_json);

  vxcore_buffer_close(ctx, buf_id);
  vxcore_string_free(buf_id);
  if (file_id) vxcore_string_free(file_id);
  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(root);
  std::cout << "  ✓ passed" << std::endl;
  return 0;
}

int test_moved_updates_file_path() {
  std::cout << "  Running test_moved_updates_file_path..." << std::endl;
  const std::string root = get_test_path("activity_move");
  cleanup_test_dir(root);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *nb_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, root.c_str(), "{\"name\":\"Mv\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &nb_id),
            VXCORE_OK);

  // Create a file, record an edit (per-file row keyed by resolved file id),
  // then rename it. The file.moved event carries oldPath/newPath and must
  // refresh the stored display path.
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, nb_id, "", "orig.md", &file_id), VXCORE_OK);
  ASSERT_NOT_NULL(file_id);
  ASSERT_EQ(vxcore_activity_record_edit(ctx, nb_id, "orig.md"), VXCORE_OK);
  // Persist the edit while "orig.md" still exists (mirrors the periodic flush
  // that runs before a later rename). Deferred file-id resolution reads the
  // live path, so the per-file row must be written before the rename.
  ASSERT_EQ(vxcore_activity_flush(ctx), VXCORE_OK);

  ASSERT_EQ(vxcore_node_rename(ctx, nb_id, "orig.md", "renamed.md"), VXCORE_OK);

  char *hist_json = nullptr;
  ASSERT_EQ(vxcore_activity_get_file_history(ctx, nb_id, file_id, &hist_json), VXCORE_OK);
  ASSERT_NOT_NULL(hist_json);
  auto jh = nlohmann::json::parse(hist_json);
  // At least one daily row exists and its path reflects the rename.
  ASSERT_TRUE(jh["daily"].size() >= 1u);
  bool sawRenamed = false;
  for (const auto &d : jh["daily"]) {
    if (d["path"].get<std::string>() == std::string("renamed.md")) sawRenamed = true;
  }
  ASSERT_TRUE(sawRenamed);
  vxcore_string_free(hist_json);

  vxcore_string_free(file_id);
  vxcore_string_free(nb_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(root);
  std::cout << "  ✓ passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  RUN_TEST(test_focus_and_record_queries);
  RUN_TEST(test_created_and_saved_events_counted);
  RUN_TEST(test_moved_updates_file_path);
  std::cout << "All test_activity tests passed" << std::endl;
  return 0;
}
