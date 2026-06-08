// test_metadata_events.cpp — Wave 2/3 of vxcore-metadata-events plan.
//
// Owns per-emit-site TDD coverage for the new metadata-change event constants
// added in T1 (event_names.h). T4 seeds the file with the persistence-layer
// event `folder.config_changed` (fired from BundledFolderManager::SaveFolderConfig).
// T5-T8 will EXTEND this file with subtests for the semantic-layer events
// (file.tagged, file.metadata_updated, file.attached, notebook.config_changed,
// etc.).
//
// Test layout matches test_event_manager.cpp:
//   - main() starts with vxcore_set_test_mode(1) so AppData → %TEMP%
//   - Each subtest returns int (0 = pass)
//   - vxcore_on_event subscribes a counter callback; payload captured for
//     per-call inspection
//   - cleanup_test_dir before AND after each test for isolation
//
// Emit pattern under test (canonical existing example at
// bundled_folder_manager.cpp:1019):
//   EmitEvent(events::kFolderConfigChanged,
//             {{"notebookId", notebook_->GetId()}, {"path", folder_path}});

#include "test_utils.h"
#include "vxcore/vxcore.h"

#include <string>
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

}  // namespace

// -------------------------------------------------------------------------
// POSITIVE: vxcore_folder_create triggers SaveFolderConfig → fires
// folder.config_changed on the success path.
//
// CreateFolder internally calls SaveFolderConfig TWICE (parent then new
// child — see bundled_folder_manager.cpp:342 + :352), so we assert >= 1
// rather than == 1, and validate every observed payload.
// -------------------------------------------------------------------------
int test_folder_config_changed_emits_on_create_folder() {
  std::cout << "  Running test_folder_config_changed_emits_on_create_folder..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("md_evt_cfg_create_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"FolderCfgCreateTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Subscribe AFTER notebook creation so the test does not race with
  // initial root vx.json writes (which are out of scope for this assertion).
  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFolderConfigChanged, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "TestFolder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  // At least one emit must have fired. CreateFolder writes parent + child
  // vx.json, so we expect >= 1 (typically 2).
  ASSERT_TRUE(cap.call_count >= 1);
  ASSERT_EQ(cap.events.size(), static_cast<size_t>(cap.call_count));
  ASSERT_EQ(cap.payloads.size(), static_cast<size_t>(cap.call_count));

  // Every observed emit must be the expected event name with a well-formed
  // payload (notebookId matches; path is a non-empty string).
  for (int i = 0; i < cap.call_count; ++i) {
    ASSERT_EQ(cap.events[i], std::string(vxcore::events::kFolderConfigChanged));
    auto j = nlohmann::json::parse(cap.payloads[i]);
    ASSERT_TRUE(j.is_object());
    ASSERT_TRUE(j.contains("notebookId"));
    ASSERT_TRUE(j.contains("path"));
    ASSERT_EQ(j["notebookId"], std::string(notebook_id));
    ASSERT_TRUE(j["path"].is_string());
    ASSERT_FALSE(j["path"].get<std::string>().empty());
  }

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_folder_config_changed_emits_on_create_folder passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// REGRESSION GUARD: when CreateFolder fails BEFORE reaching SaveFolderConfig
// (e.g., invalid parent path → VXCORE_ERR_NOT_FOUND from GetFolderConfig),
// no folder.config_changed emit must fire. This pins the contract that the
// emit lives ONLY on the success path.
// -------------------------------------------------------------------------
int test_folder_config_changed_no_emit_on_failure() {
  std::cout << "  Running test_folder_config_changed_no_emit_on_failure..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("md_evt_cfg_fail_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"FolderCfgFailTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFolderConfigChanged, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  // Parent does not exist. GetFolderConfig returns VXCORE_ERR_NOT_FOUND
  // BEFORE any SaveFolderConfig call is reached.
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, "nonexistent/path/that/cant/exist",
                             "TestFolder", &folder_id);
  ASSERT_NE(err, VXCORE_OK);
  // folder_id must not have been assigned on the failure path.
  ASSERT_NULL(folder_id);

  // No emit must have fired.
  ASSERT_EQ(cap.call_count, 0);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_folder_config_changed_no_emit_on_failure passed" << std::endl;
  return 0;
}

int main() {
  // T4 subtests (folder.config_changed persistence event).
  RUN_TEST(test_folder_config_changed_emits_on_create_folder);
  RUN_TEST(test_folder_config_changed_no_emit_on_failure);

  // T5-T8 will append additional RUN_TEST(...) calls here for the semantic
  // events (file.tagged, file.metadata_updated, file.attached, etc.) and the
  // notebook.config_changed persistence event.

  std::cout << "All metadata event tests passed!" << std::endl;
  return 0;
}
