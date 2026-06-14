// test_folder_child_order.cpp — Tasks T1+T5 of notebook-explorer-drag-reorder.
//
// Covers the new public C API vxcore_folder_set_children_order, which
// atomically rewrites the order of `FolderConfig.folders` / `FolderConfig.files`
// in a folder's vx.json after validating that the submitted lists are exact
// permutations of existing children.
//
// 10 sub-tests:
//   1. Reorder folders only.
//   2. Reorder files only.
//   3. Reorder both atomically.
//   4. Incomplete permutation -> VXCORE_ERR_PERMUTATION_MISMATCH; vx.json bytes
//      unchanged.
//   5. Foreign name -> VXCORE_ERR_PERMUTATION_MISMATCH; disk unchanged.
//   6. No-op (current order unchanged) -> VXCORE_OK; mtime unchanged.
//   7. Null params -> VXCORE_ERR_INVALID_PARAM.
//   8. Malformed JSON -> VXCORE_ERR_INVALID_PARAM.
//   9. Empty folder_path ("" or ".") operates on notebook root.
//  10. Event fires on success only (subscribe via vxcore_on_event; expect
//      callback count == 1 across one successful reorder + one no-op + one
//      permutation-mismatch).

#include "test_utils.h"
#include "vxcore/vxcore.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/event_names.h"

namespace {

// Per-test state captured by the subscriber.
struct EventCapture {
  int call_count = 0;
  std::vector<std::string> events;
  std::vector<std::string> payloads;
};

void capture_event_cb(const char *event_name, const char *json_data, void *userdata) {
  auto *cap = static_cast<EventCapture *>(userdata);
  cap->call_count++;
  cap->events.emplace_back(event_name ? event_name : "");
  cap->payloads.emplace_back(json_data ? json_data : "");
}

// Read all bytes of a UTF-8 path into a std::string. Returns empty string on
// failure (callers can check via .empty() if the path is expected non-empty).
std::string read_file_bytes(const std::string &utf8_path) {
  std::ifstream f(utf8_to_fs_path(utf8_path), std::ios::binary);
  if (!f.is_open()) {
    return {};
  }
  std::ostringstream oss;
  oss << f.rdbuf();
  return oss.str();
}

// Build the absolute vx.json path for the root of a bundled notebook.
std::string root_vx_json_path(const std::string &notebook_root) {
  return notebook_root + "/vx_notebook/contents/vx.json";
}

// Build the absolute vx.json path for a subfolder inside a bundled notebook.
std::string sub_vx_json_path(const std::string &notebook_root,
                             const std::string &folder_rel_path) {
  return notebook_root + "/vx_notebook/contents/" + folder_rel_path + "/vx.json";
}

// Helper: create a notebook with the supplied folders/files seeded at root.
// Returns out_notebook_id (caller frees) and out_ctx (caller destroys).
VxCoreError seed_notebook(const std::string &nb_path,
                          const std::vector<std::string> &folders,
                          const std::vector<std::string> &files,
                          VxCoreContextHandle *out_ctx, char **out_notebook_id) {
  *out_ctx = nullptr;
  *out_notebook_id = nullptr;

  VxCoreError err = vxcore_context_create(nullptr, out_ctx);
  if (err != VXCORE_OK) {
    return err;
  }
  err = vxcore_notebook_create(*out_ctx, nb_path.c_str(),
                               "{\"name\":\"ReorderTestNotebook\"}",
                               VXCORE_NOTEBOOK_BUNDLED, out_notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  // Seed folders at root.
  for (const auto &folder : folders) {
    char *folder_id = nullptr;
    err = vxcore_folder_create(*out_ctx, *out_notebook_id, ".", folder.c_str(), &folder_id);
    if (err != VXCORE_OK) {
      return err;
    }
    vxcore_string_free(folder_id);
  }
  // Seed files at root.
  for (const auto &file : files) {
    char *file_id = nullptr;
    err = vxcore_file_create(*out_ctx, *out_notebook_id, ".", file.c_str(), &file_id);
    if (err != VXCORE_OK) {
      return err;
    }
    vxcore_string_free(file_id);
  }
  return VXCORE_OK;
}

// Helper: read root folder via list_children and return the order vectors.
VxCoreError read_root_order(VxCoreContextHandle ctx, const char *notebook_id,
                            std::vector<std::string> &out_folders,
                            std::vector<std::string> &out_files) {
  out_folders.clear();
  out_files.clear();
  char *children_json = nullptr;
  VxCoreError err = vxcore_folder_list_children(ctx, notebook_id, ".", &children_json);
  if (err != VXCORE_OK) {
    return err;
  }
  try {
    auto j = nlohmann::json::parse(children_json);
    if (j.contains("folders") && j["folders"].is_array()) {
      for (const auto &item : j["folders"]) {
        if (item.contains("name")) {
          out_folders.push_back(item["name"].get<std::string>());
        }
      }
    }
    if (j.contains("files") && j["files"].is_array()) {
      for (const auto &item : j["files"]) {
        if (item.contains("name")) {
          out_files.push_back(item["name"].get<std::string>());
        }
      }
    }
  } catch (const std::exception &) {
    vxcore_string_free(children_json);
    return VXCORE_ERR_JSON_PARSE;
  }
  vxcore_string_free(children_json);
  return VXCORE_OK;
}

}  // namespace

// -------------------------------------------------------------------------
// Test 1: Reorder folders only.
// -------------------------------------------------------------------------
int test_reorder_folders_only() {
  std::cout << "  Running test_reorder_folders_only..." << std::endl;
  std::string nb_path = get_test_path("fco_folders_only_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  char *notebook_id = nullptr;
  VxCoreError err = seed_notebook(nb_path, {"a", "b", "c"}, {}, &ctx, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify seed order is ["a","b","c"] before reordering.
  std::vector<std::string> before_folders, before_files;
  err = read_root_order(ctx, notebook_id, before_folders, before_files);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(before_folders.size(), static_cast<size_t>(3));
  ASSERT_EQ(before_folders[0], std::string("a"));
  ASSERT_EQ(before_folders[1], std::string("b"));
  ASSERT_EQ(before_folders[2], std::string("c"));

  // Reorder to ["c","a","b"].
  err = vxcore_folder_set_children_order(ctx, notebook_id, ".",
                                          R"({"folders":["c","a","b"]})");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify new disk order.
  std::vector<std::string> after_folders, after_files;
  err = read_root_order(ctx, notebook_id, after_folders, after_files);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(after_folders.size(), static_cast<size_t>(3));
  ASSERT_EQ(after_folders[0], std::string("c"));
  ASSERT_EQ(after_folders[1], std::string("a"));
  ASSERT_EQ(after_folders[2], std::string("b"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  PASS test_reorder_folders_only" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// Test 2: Reorder files only.
// -------------------------------------------------------------------------
int test_reorder_files_only() {
  std::cout << "  Running test_reorder_files_only..." << std::endl;
  std::string nb_path = get_test_path("fco_files_only_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  char *notebook_id = nullptr;
  VxCoreError err =
      seed_notebook(nb_path, {}, {"x.md", "y.md", "z.md"}, &ctx, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_folder_set_children_order(ctx, notebook_id, ".",
                                          R"({"files":["z.md","y.md","x.md"]})");
  ASSERT_EQ(err, VXCORE_OK);

  std::vector<std::string> after_folders, after_files;
  err = read_root_order(ctx, notebook_id, after_folders, after_files);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(after_files.size(), static_cast<size_t>(3));
  ASSERT_EQ(after_files[0], std::string("z.md"));
  ASSERT_EQ(after_files[1], std::string("y.md"));
  ASSERT_EQ(after_files[2], std::string("x.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  PASS test_reorder_files_only" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// Test 3: Reorder both atomically (single vx.json write).
// -------------------------------------------------------------------------
int test_reorder_both_atomically() {
  std::cout << "  Running test_reorder_both_atomically..." << std::endl;
  std::string nb_path = get_test_path("fco_both_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  char *notebook_id = nullptr;
  VxCoreError err = seed_notebook(nb_path, {"a", "b"}, {"x.md", "y.md"}, &ctx, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_folder_set_children_order(
      ctx, notebook_id, ".",
      R"({"folders":["b","a"],"files":["y.md","x.md"]})");
  ASSERT_EQ(err, VXCORE_OK);

  std::vector<std::string> after_folders, after_files;
  err = read_root_order(ctx, notebook_id, after_folders, after_files);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(after_folders.size(), static_cast<size_t>(2));
  ASSERT_EQ(after_folders[0], std::string("b"));
  ASSERT_EQ(after_folders[1], std::string("a"));
  ASSERT_EQ(after_files.size(), static_cast<size_t>(2));
  ASSERT_EQ(after_files[0], std::string("y.md"));
  ASSERT_EQ(after_files[1], std::string("x.md"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  PASS test_reorder_both_atomically" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// Test 4: Incomplete permutation -> VXCORE_ERR_PERMUTATION_MISMATCH; bytes
//         of vx.json unchanged.
// -------------------------------------------------------------------------
int test_incomplete_permutation_rejected() {
  std::cout << "  Running test_incomplete_permutation_rejected..." << std::endl;
  std::string nb_path = get_test_path("fco_incomplete_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  char *notebook_id = nullptr;
  VxCoreError err = seed_notebook(nb_path, {"a", "b", "c"}, {}, &ctx, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string vx_json = root_vx_json_path(nb_path);
  std::string before = read_file_bytes(vx_json);
  ASSERT_FALSE(before.empty());

  // Missing "c" — not an exact permutation.
  err = vxcore_folder_set_children_order(ctx, notebook_id, ".",
                                          R"({"folders":["a","b"]})");
  ASSERT_EQ(err, VXCORE_ERR_PERMUTATION_MISMATCH);

  std::string after = read_file_bytes(vx_json);
  ASSERT_EQ(before, after);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  PASS test_incomplete_permutation_rejected" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// Test 5: Foreign name -> VXCORE_ERR_PERMUTATION_MISMATCH; disk unchanged.
// -------------------------------------------------------------------------
int test_foreign_name_rejected() {
  std::cout << "  Running test_foreign_name_rejected..." << std::endl;
  std::string nb_path = get_test_path("fco_foreign_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  char *notebook_id = nullptr;
  VxCoreError err = seed_notebook(nb_path, {"a", "b", "c"}, {}, &ctx, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string vx_json = root_vx_json_path(nb_path);
  std::string before = read_file_bytes(vx_json);
  ASSERT_FALSE(before.empty());

  // "d" does not exist — foreign name.
  err = vxcore_folder_set_children_order(ctx, notebook_id, ".",
                                          R"({"folders":["a","b","d"]})");
  ASSERT_EQ(err, VXCORE_ERR_PERMUTATION_MISMATCH);

  std::string after = read_file_bytes(vx_json);
  ASSERT_EQ(before, after);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  PASS test_foreign_name_rejected" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// Test 6: No-op (current order unchanged) -> VXCORE_OK; mtime unchanged.
// -------------------------------------------------------------------------
int test_noop_does_not_write() {
  std::cout << "  Running test_noop_does_not_write..." << std::endl;
  std::string nb_path = get_test_path("fco_noop_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  char *notebook_id = nullptr;
  VxCoreError err = seed_notebook(nb_path, {"a", "b", "c"}, {}, &ctx, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string vx_json = root_vx_json_path(nb_path);
  auto vx_fs = utf8_to_fs_path(vx_json);
  ASSERT_TRUE(std::filesystem::exists(vx_fs));
  auto mtime_before = std::filesystem::last_write_time(vx_fs);

  // Sleep so any subsequent write would show a different mtime even on
  // filesystems with coarse-grained timestamps.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Submit current order unchanged.
  err = vxcore_folder_set_children_order(ctx, notebook_id, ".",
                                          R"({"folders":["a","b","c"]})");
  ASSERT_EQ(err, VXCORE_OK);

  auto mtime_after = std::filesystem::last_write_time(vx_fs);
  ASSERT_TRUE(mtime_before == mtime_after);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  PASS test_noop_does_not_write" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// Test 7: Null params -> VXCORE_ERR_INVALID_PARAM.
// -------------------------------------------------------------------------
int test_null_params_rejected() {
  std::cout << "  Running test_null_params_rejected..." << std::endl;
  std::string nb_path = get_test_path("fco_null_params_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  char *notebook_id = nullptr;
  VxCoreError err = seed_notebook(nb_path, {"a"}, {}, &ctx, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT_EQ(vxcore_folder_set_children_order(nullptr, notebook_id, ".", "{}"),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_set_children_order(ctx, nullptr, ".", "{}"),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_set_children_order(ctx, notebook_id, nullptr, "{}"),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_folder_set_children_order(ctx, notebook_id, ".", nullptr),
            VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  PASS test_null_params_rejected" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// Test 8: Malformed JSON -> VXCORE_ERR_INVALID_PARAM.
// -------------------------------------------------------------------------
int test_malformed_json_rejected() {
  std::cout << "  Running test_malformed_json_rejected..." << std::endl;
  std::string nb_path = get_test_path("fco_malformed_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  char *notebook_id = nullptr;
  VxCoreError err = seed_notebook(nb_path, {"a"}, {}, &ctx, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Truly malformed JSON.
  ASSERT_EQ(vxcore_folder_set_children_order(ctx, notebook_id, ".", "not{json"),
            VXCORE_ERR_INVALID_PARAM);
  // Top-level non-object (parses cleanly but the shape is wrong).
  ASSERT_EQ(vxcore_folder_set_children_order(ctx, notebook_id, ".", "[1,2,3]"),
            VXCORE_ERR_INVALID_PARAM);
  // "folders" entry is not an array.
  ASSERT_EQ(vxcore_folder_set_children_order(ctx, notebook_id, ".",
                                              R"({"folders":42})"),
            VXCORE_ERR_INVALID_PARAM);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  PASS test_malformed_json_rejected" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// Test 9: Empty folder_path ("" or ".") operates on notebook root.
// -------------------------------------------------------------------------
int test_empty_path_targets_root() {
  std::cout << "  Running test_empty_path_targets_root..." << std::endl;
  std::string nb_path = get_test_path("fco_empty_path_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  char *notebook_id = nullptr;
  VxCoreError err = seed_notebook(nb_path, {"a", "b"}, {}, &ctx, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Empty string -> root.
  err = vxcore_folder_set_children_order(ctx, notebook_id, "",
                                          R"({"folders":["b","a"]})");
  ASSERT_EQ(err, VXCORE_OK);

  std::vector<std::string> folders, files;
  err = read_root_order(ctx, notebook_id, folders, files);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(folders.size(), static_cast<size_t>(2));
  ASSERT_EQ(folders[0], std::string("b"));
  ASSERT_EQ(folders[1], std::string("a"));

  // Then "." -> root, reorder back to original.
  err = vxcore_folder_set_children_order(ctx, notebook_id, ".",
                                          R"({"folders":["a","b"]})");
  ASSERT_EQ(err, VXCORE_OK);

  err = read_root_order(ctx, notebook_id, folders, files);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(folders.size(), static_cast<size_t>(2));
  ASSERT_EQ(folders[0], std::string("a"));
  ASSERT_EQ(folders[1], std::string("b"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  PASS test_empty_path_targets_root" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// Test 10: Event fires on success only. Across 1 successful reorder + 1
//          no-op + 1 permutation-mismatch the subscribed callback must be
//          invoked exactly once.
// -------------------------------------------------------------------------
int test_event_fires_on_success_only() {
  std::cout << "  Running test_event_fires_on_success_only..." << std::endl;
  std::string nb_path = get_test_path("fco_event_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  char *notebook_id = nullptr;
  VxCoreError err = seed_notebook(nb_path, {"a", "b", "c"}, {}, &ctx, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFolderChildrenReordered, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  // 1) Successful reorder — MUST emit once.
  err = vxcore_folder_set_children_order(ctx, notebook_id, ".",
                                          R"({"folders":["c","a","b"]})");
  ASSERT_EQ(err, VXCORE_OK);

  // 2) No-op (resubmit current order) — MUST NOT emit.
  err = vxcore_folder_set_children_order(ctx, notebook_id, ".",
                                          R"({"folders":["c","a","b"]})");
  ASSERT_EQ(err, VXCORE_OK);

  // 3) Permutation mismatch — MUST NOT emit.
  err = vxcore_folder_set_children_order(ctx, notebook_id, ".",
                                          R"({"folders":["c","a","d"]})");
  ASSERT_EQ(err, VXCORE_ERR_PERMUTATION_MISMATCH);

  ASSERT_EQ(cap.call_count, 1);
  ASSERT_EQ(cap.events.size(), static_cast<size_t>(1));
  ASSERT_EQ(cap.events[0], std::string(vxcore::events::kFolderChildrenReordered));

  // Validate payload shape.
  auto payload = nlohmann::json::parse(cap.payloads[0]);
  ASSERT_TRUE(payload.is_object());
  ASSERT_TRUE(payload.contains("notebookId"));
  ASSERT_TRUE(payload.contains("path"));
  ASSERT_TRUE(payload.contains("folders"));
  ASSERT_EQ(payload["notebookId"], std::string(notebook_id));
  ASSERT_TRUE(payload["path"].is_string());
  ASSERT_TRUE(payload["folders"].is_array());
  ASSERT_EQ(payload["folders"].size(), static_cast<size_t>(3));
  ASSERT_EQ(payload["folders"][0], std::string("c"));
  ASSERT_EQ(payload["folders"][1], std::string("a"));
  ASSERT_EQ(payload["folders"][2], std::string("b"));

  vxcore_off_event(ctx, vxcore::events::kFolderChildrenReordered, capture_event_cb);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  PASS test_event_fires_on_success_only" << std::endl;
  return 0;
}

int main() {
  // MUST be the first line per vxcore tests AGENTS.md — without test mode the
  // suite writes to real AppData and pollutes the user's machine.
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  RUN_TEST(test_reorder_folders_only);
  RUN_TEST(test_reorder_files_only);
  RUN_TEST(test_reorder_both_atomically);
  RUN_TEST(test_incomplete_permutation_rejected);
  RUN_TEST(test_foreign_name_rejected);
  RUN_TEST(test_noop_does_not_write);
  RUN_TEST(test_null_params_rejected);
  RUN_TEST(test_malformed_json_rejected);
  RUN_TEST(test_empty_path_targets_root);
  RUN_TEST(test_event_fires_on_success_only);

  std::cout << "All test_folder_child_order sub-tests passed." << std::endl;
  return 0;
}
