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

// -------------------------------------------------------------------------
// T6 POSITIVE: vxcore_node_update_metadata on a FILE fires
// file.metadata_updated exactly once on the success path with payload
// {notebookId, path}. The path is notebook-relative (matches the path arg
// passed to UpdateFileMetadata, cleaned via GetCleanRelativePath).
// We subscribe ONLY to kFileMetadataUpdated, so folder.config_changed
// (fired transitively by SaveFolderConfig) does not pollute the count.
// -------------------------------------------------------------------------
int test_file_metadata_updated_emits_on_update_file_metadata() {
  std::cout << "  Running test_file_metadata_updated_emits_on_update_file_metadata..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("md_evt_file_meta_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"FileMetaTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Create the file BEFORE subscribing so file.created / folder.config_changed
  // emits do not race with our assertion.
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFileMetadataUpdated, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  // Valid JSON object for the metadata blob.
  err = vxcore_node_update_metadata(ctx, notebook_id, "note.md", "{\"author\":\"alice\"}");
  ASSERT_EQ(err, VXCORE_OK);

  // Exactly one file.metadata_updated emit.
  ASSERT_EQ(cap.call_count, 1);
  ASSERT_EQ(cap.events.size(), static_cast<size_t>(1));
  ASSERT_EQ(cap.payloads.size(), static_cast<size_t>(1));
  ASSERT_EQ(cap.events[0], std::string(vxcore::events::kFileMetadataUpdated));

  auto j = nlohmann::json::parse(cap.payloads[0]);
  ASSERT_TRUE(j.is_object());
  ASSERT_TRUE(j.contains("notebookId"));
  ASSERT_TRUE(j.contains("path"));
  ASSERT_EQ(j["notebookId"], std::string(notebook_id));
  ASSERT_TRUE(j["path"].is_string());
  ASSERT_EQ(j["path"].get<std::string>(), std::string("note.md"));
  // Payload MUST NOT leak the metadata blob itself (subscribers re-read).
  ASSERT_FALSE(j.contains("metadata"));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_file_metadata_updated_emits_on_update_file_metadata passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// T6 POSITIVE: vxcore_node_update_metadata on a FOLDER fires
// folder.metadata_updated exactly once with payload {notebookId, path}.
// Verifies `path` is the FOLDER path (not the parent folder).
// -------------------------------------------------------------------------
int test_folder_metadata_updated_emits_on_update_folder_metadata() {
  std::cout << "  Running test_folder_metadata_updated_emits_on_update_folder_metadata..."
            << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("md_evt_folder_meta_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"FolderMetaTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Create the folder BEFORE subscribing so folder.created and the parent +
  // child folder.config_changed emits do not race with our assertion.
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "MyFolder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFolderMetadataUpdated, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_node_update_metadata(ctx, notebook_id, "MyFolder", "{\"color\":\"blue\"}");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT_EQ(cap.call_count, 1);
  ASSERT_EQ(cap.events.size(), static_cast<size_t>(1));
  ASSERT_EQ(cap.payloads.size(), static_cast<size_t>(1));
  ASSERT_EQ(cap.events[0], std::string(vxcore::events::kFolderMetadataUpdated));

  auto j = nlohmann::json::parse(cap.payloads[0]);
  ASSERT_TRUE(j.is_object());
  ASSERT_TRUE(j.contains("notebookId"));
  ASSERT_TRUE(j.contains("path"));
  ASSERT_EQ(j["notebookId"], std::string(notebook_id));
  ASSERT_TRUE(j["path"].is_string());
  // The path MUST be the folder path itself (NOT the parent ".").
  ASSERT_EQ(j["path"].get<std::string>(), std::string("MyFolder"));
  // Payload MUST NOT leak the metadata blob itself.
  ASSERT_FALSE(j.contains("metadata"));

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_folder_metadata_updated_emits_on_update_folder_metadata passed"
            << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// T6 REGRESSION GUARD: malformed JSON → UpdateFileMetadata returns
// VXCORE_ERR_JSON_PARSE BEFORE SaveFolderConfig runs → no emit fires for
// EITHER file.metadata_updated OR folder.metadata_updated.
// -------------------------------------------------------------------------
int test_no_emit_on_invalid_metadata_json() {
  std::cout << "  Running test_no_emit_on_invalid_metadata_json..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("md_evt_bad_json_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"BadJsonTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Need a real file for DetectNodeType to route into UpdateFileMetadata's
  // JSON-parse branch (rather than failing earlier with NOT_FOUND).
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFileMetadataUpdated, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_on_event(ctx, vxcore::events::kFolderMetadataUpdated, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  // Malformed JSON: missing closing brace. UpdateFileMetadata catches the
  // nlohmann parse exception and returns VXCORE_ERR_JSON_PARSE.
  err = vxcore_node_update_metadata(ctx, notebook_id, "note.md", "{not valid json");
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);

  // No emit must have fired for either event.
  ASSERT_EQ(cap.call_count, 0);

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_no_emit_on_invalid_metadata_json passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// T6 REGRESSION GUARD: vxcore_node_update_metadata on a missing file
// returns an error (DetectNodeType or UpdateFileMetadata) BEFORE
// SaveFolderConfig runs → no emit fires for EITHER metadata-updated event.
// -------------------------------------------------------------------------
int test_no_emit_on_nonexistent_file_metadata() {
  std::cout << "  Running test_no_emit_on_nonexistent_file_metadata..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("md_evt_missing_file_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"MissingFileTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFileMetadataUpdated, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_on_event(ctx, vxcore::events::kFolderMetadataUpdated, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  // File never created. Even with valid JSON, the update must fail before
  // any SaveFolderConfig write.
  err = vxcore_node_update_metadata(ctx, notebook_id, "nope.md", "{\"author\":\"alice\"}");
  ASSERT_NE(err, VXCORE_OK);

  ASSERT_EQ(cap.call_count, 0);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_no_emit_on_nonexistent_file_metadata passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// T8 POSITIVE: vxcore_notebook_update_config drives
// BundledNotebook::UpdateConfig, which must fire kNotebookConfigChanged
// EXACTLY ONCE on the success path with payload {notebookId: <id>} (and
// nothing else — `path` is intentionally OMITTED because this IS the
// notebook config; subscribers re-read on demand).
//
// Internal call shape: vxcore_notebook_update_config
//   → NotebookManager::UpdateNotebookConfig (parses JSON → NotebookConfig)
//   → notebook->UpdateConfig(config)  ← the BundledNotebook override emits
//   → NotebookManager::UpdateNotebookRecord (writes session record, no emit)
// -------------------------------------------------------------------------
int test_notebook_config_changed_emits_on_update_config() {
  std::cout << "  Running test_notebook_config_changed_emits_on_update_config..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("md_evt_nb_cfg_update");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"NotebookCfgUpdateTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Subscribe AFTER notebook creation so we do not race with the initial
  // UpdateConfig call inside InitOnCreation (which runs before
  // NotebookManager wires the event_manager — see T2/T13 notes).
  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kNotebookConfigChanged, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_notebook_update_config(
      ctx, notebook_id, "{\"name\":\"Renamed Notebook\",\"description\":\"Updated\"}");
  ASSERT_EQ(err, VXCORE_OK);

  // EXACTLY one emit — UpdateNotebookRecord does not emit, only
  // BundledNotebook::UpdateConfig does.
  ASSERT_EQ(cap.call_count, 1);
  ASSERT_EQ(cap.events.size(), static_cast<size_t>(1));
  ASSERT_EQ(cap.payloads.size(), static_cast<size_t>(1));
  ASSERT_EQ(cap.events[0], std::string(vxcore::events::kNotebookConfigChanged));

  auto j = nlohmann::json::parse(cap.payloads[0]);
  ASSERT_TRUE(j.is_object());
  ASSERT_TRUE(j.contains("notebookId"));
  ASSERT_EQ(j["notebookId"], std::string(notebook_id));
  // Contract: NO `path` field — it's THE notebook config, not a sub-file.
  ASSERT_FALSE(j.contains("path"));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_notebook_config_changed_emits_on_update_config passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// T8 REGRESSION GUARD (nullptr-safe / wiring sanity):
//
// The strictest version of this test would directly construct a
// BundledNotebook in C++, leave its inherited `event_manager_` field at
// its default `nullptr`, call `UpdateConfig` and assert no crash AND
// 0 emits to a previously-installed subscriber. That is NOT feasible from
// this test target: `test_metadata_events` is registered with
// `add_vxcore_test()` (NO `INTERNALS` flag in libs/vxcore/tests/CMakeLists.txt
// line 45), so it links only against `vxcore.dll`'s VXCORE_API-exported
// surface. `BundledNotebook` is not VXCORE_API-decorated AND its constructor
// is `private`, accessible only via the static `Create`/`Open` factories
// that are themselves managed by `NotebookManager`. By the time any
// public C API can reach a BundledNotebook's UpdateConfig, the manager
// has already wired event_manager_ via the propagation loop landed in T2
// (`notebook_manager.cpp:124-125` / `:180-181`).
//
// We therefore degrade this test (per the task spec's explicit fallback)
// into a wiring sanity-check: the emit MUST land when the public API
// reaches UpdateConfig. The nullptr branch (`if (event_manager_ != nullptr)`
// guard in bundled_notebook.cpp::UpdateConfig) is exercised in production
// during BundledNotebook::InitOnCreation's pre-wire call, which is
// covered separately by T13's regression test on creation-timing.
// -------------------------------------------------------------------------
int test_notebook_config_changed_no_emit_when_event_manager_absent() {
  std::cout << "  Running test_notebook_config_changed_no_emit_when_event_manager_absent..."
            << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("md_evt_nb_cfg_wired");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"NotebookCfgWiredTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kNotebookConfigChanged, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  // (a) Public API call succeeds (process did NOT crash even though the
  // emit path runs through the nullptr-guard check internally on the
  // first InitOnCreation write — proves the guard is in place).
  err = vxcore_notebook_update_config(ctx, notebook_id, "{\"name\":\"PostWireUpdate\"}");
  ASSERT_EQ(err, VXCORE_OK);

  // (b) Subscriber received the emit when event_manager IS wired (the
  // post-T2 normal path).
  ASSERT_EQ(cap.call_count, 1);
  ASSERT_EQ(cap.events[0], std::string(vxcore::events::kNotebookConfigChanged));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_notebook_config_changed_no_emit_when_event_manager_absent passed"
            << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// T8 REGRESSION GUARD: when the disk write fails (e.g., parent metadata
// folder was destroyed out from under the live notebook), UpdateConfig
// must return a non-OK error AND must NOT emit kNotebookConfigChanged.
// This pins the contract that the emit lives ONLY on the success branch,
// AFTER `file.is_open()` and the JSON write succeed.
//
// Strategy: create the notebook, then delete the entire notebook root
// folder out-of-band so the metadata directory (<root>/vx_notebook/)
// no longer exists. The next vxcore_notebook_update_config call drives
// `std::ofstream` open on a missing parent directory — on Windows and
// POSIX alike, the stream stays closed and BundledNotebook::UpdateConfig
// returns VXCORE_ERR_IO BEFORE reaching the emit site.
// -------------------------------------------------------------------------
int test_notebook_config_changed_no_emit_on_write_failure() {
  std::cout << "  Running test_notebook_config_changed_no_emit_on_write_failure..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("md_evt_nb_cfg_writefail");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"NotebookCfgWriteFailTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kNotebookConfigChanged, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  // Remove the entire notebook root (including <root>/vx_notebook/) so the
  // subsequent UpdateConfig cannot open the config.json for write.
  cleanup_test_dir(nb_path);

  err = vxcore_notebook_update_config(ctx, notebook_id, "{\"name\":\"DoomedUpdate\"}");
  ASSERT_NE(err, VXCORE_OK);

  // No emit must have fired on the failure path.
  ASSERT_EQ(cap.call_count, 0);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_notebook_config_changed_no_emit_on_write_failure passed" << std::endl;
  return 0;
}

// =========================================================================
// T5 subtests — Semantic tag events.
//
// Owns coverage for kFileTagged / kFileUntagged / kFileTagsReplaced.
// Each emit must fire EXACTLY ONCE on the success path AFTER both
// SaveFolderConfig (disk write) AND MetadataStore write-through succeed.
// Idempotent / failure paths MUST NOT emit.
// =========================================================================

namespace {

// Shared setup helper: create a bundled notebook + one file at notebook root.
// Returns the absolute notebook path so the caller can cleanup_test_dir(...)
// after destroying the context. notebook_id and file_id are caller-owned;
// caller must vxcore_string_free(...) both.
//
// On any failure, sets *out_err and returns empty string. The caller is
// expected to ASSERT_EQ(err, VXCORE_OK) before proceeding.
std::string make_notebook_with_file(VxCoreContextHandle ctx, const std::string &nb_basename,
                                    const std::string &file_name, char **out_notebook_id,
                                    char **out_file_id, VxCoreError *out_err) {
  std::string nb_path = get_test_path(nb_basename);
  cleanup_test_dir(nb_path);
  std::string json_payload = "{\"name\":\"" + nb_basename + "\"}";
  *out_err = vxcore_notebook_create(ctx, nb_path.c_str(), json_payload.c_str(),
                                    VXCORE_NOTEBOOK_BUNDLED, out_notebook_id);
  if (*out_err != VXCORE_OK || *out_notebook_id == nullptr) {
    return std::string();
  }
  *out_err = vxcore_file_create(ctx, *out_notebook_id, ".", file_name.c_str(), out_file_id);
  if (*out_err != VXCORE_OK || *out_file_id == nullptr) {
    return std::string();
  }
  return nb_path;
}

}  // namespace

// -------------------------------------------------------------------------
// POSITIVE: vxcore_file_tag → TagFile success path fires file.tagged exactly
// once with payload {notebookId, path, tag}.
// -------------------------------------------------------------------------
int test_file_tagged_emits_on_tag_file() {
  std::cout << "  Running test_file_tagged_emits_on_tag_file..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  char *file_id = nullptr;
  std::string nb_path = make_notebook_with_file(ctx, "md_evt_tag_ok_nb", "note.md", &notebook_id,
                                                &file_id, &err);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);
  ASSERT_NOT_NULL(file_id);

  // Define the tag at notebook level so TagFile's notebook_->FindTag() passes.
  err = vxcore_tag_create(ctx, notebook_id, "important");
  ASSERT_EQ(err, VXCORE_OK);

  // Subscribe AFTER all setup writes (notebook create, file create, tag
  // create) so we only see the one emit from vxcore_file_tag below.
  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFileTagged, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "note.md", "important");
  ASSERT_EQ(err, VXCORE_OK);

  // TagFile calls SaveFolderConfig ONCE (only the containing folder is
  // modified), so the emit must fire EXACTLY once.
  ASSERT_EQ(cap.call_count, 1);
  ASSERT_EQ(cap.events[0], std::string(vxcore::events::kFileTagged));

  auto j = nlohmann::json::parse(cap.payloads[0]);
  ASSERT_TRUE(j.is_object());
  ASSERT_EQ(j["notebookId"], std::string(notebook_id));
  ASSERT_EQ(j["path"], std::string("note.md"));
  ASSERT_EQ(j["tag"], std::string("important"));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_file_tagged_emits_on_tag_file passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// POSITIVE: vxcore_file_untag → UntagFile success path fires file.untagged
// exactly once with payload {notebookId, path, tag}.
// -------------------------------------------------------------------------
int test_file_untagged_emits_on_untag_file() {
  std::cout << "  Running test_file_untagged_emits_on_untag_file..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  char *file_id = nullptr;
  std::string nb_path = make_notebook_with_file(ctx, "md_evt_untag_ok_nb", "note.md",
                                                &notebook_id, &file_id, &err);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_tag_create(ctx, notebook_id, "review");
  ASSERT_EQ(err, VXCORE_OK);

  // Apply the tag BEFORE subscribing so the kFileTagged emit (issued by
  // TagFile) does not pollute later counts. We are only measuring untag.
  err = vxcore_file_tag(ctx, notebook_id, "note.md", "review");
  ASSERT_EQ(err, VXCORE_OK);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFileUntagged, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_untag(ctx, notebook_id, "note.md", "review");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT_EQ(cap.call_count, 1);
  ASSERT_EQ(cap.events[0], std::string(vxcore::events::kFileUntagged));

  auto j = nlohmann::json::parse(cap.payloads[0]);
  ASSERT_TRUE(j.is_object());
  ASSERT_EQ(j["notebookId"], std::string(notebook_id));
  ASSERT_EQ(j["path"], std::string("note.md"));
  ASSERT_EQ(j["tag"], std::string("review"));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_file_untagged_emits_on_untag_file passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// REGRESSION GUARD: vxcore_file_untag for a tag NOT in the file's tags list
// returns VXCORE_ERR_NOT_FOUND and MUST NOT emit. This pins the rule that
// the emit lives ONLY after the SaveFolderConfig + MetadataStore writes,
// past the idempotent early-return in UntagFile.
// -------------------------------------------------------------------------
int test_file_untagged_no_emit_when_tag_not_present() {
  std::cout << "  Running test_file_untagged_no_emit_when_tag_not_present..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  char *file_id = nullptr;
  std::string nb_path = make_notebook_with_file(ctx, "md_evt_untag_miss_nb", "note.md",
                                                &notebook_id, &file_id, &err);
  ASSERT_EQ(err, VXCORE_OK);

  // Define the tag so the only failure path is "tag not in file's list".
  err = vxcore_tag_create(ctx, notebook_id, "draft");
  ASSERT_EQ(err, VXCORE_OK);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFileUntagged, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  // File has no tags → UntagFile finds tag missing → returns NOT_FOUND.
  err = vxcore_file_untag(ctx, notebook_id, "note.md", "draft");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  ASSERT_EQ(cap.call_count, 0);

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_file_untagged_no_emit_when_tag_not_present passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// POSITIVE: vxcore_file_update_tags → UpdateFileTags success path fires
// file.tags_replaced exactly once with payload {notebookId, path, tags}
// where `tags` is the EXACT JSON array of new tag names.
// -------------------------------------------------------------------------
int test_file_tags_replaced_emits_on_update_tags() {
  std::cout << "  Running test_file_tags_replaced_emits_on_update_tags..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  char *file_id = nullptr;
  std::string nb_path = make_notebook_with_file(ctx, "md_evt_replace_ok_nb", "note.md",
                                                &notebook_id, &file_id, &err);
  ASSERT_EQ(err, VXCORE_OK);

  // Define both tags at notebook level so UpdateFileTags's per-tag FindTag
  // loop passes for the whole input array.
  err = vxcore_tag_create(ctx, notebook_id, "alpha");
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_tag_create(ctx, notebook_id, "beta");
  ASSERT_EQ(err, VXCORE_OK);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFileTagsReplaced, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  const char *tags_payload = "[\"alpha\",\"beta\"]";
  err = vxcore_file_update_tags(ctx, notebook_id, "note.md", tags_payload);
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT_EQ(cap.call_count, 1);
  ASSERT_EQ(cap.events[0], std::string(vxcore::events::kFileTagsReplaced));

  auto j = nlohmann::json::parse(cap.payloads[0]);
  ASSERT_TRUE(j.is_object());
  ASSERT_EQ(j["notebookId"], std::string(notebook_id));
  ASSERT_EQ(j["path"], std::string("note.md"));
  ASSERT_TRUE(j["tags"].is_array());
  ASSERT_EQ(j["tags"].size(), static_cast<size_t>(2));
  ASSERT_EQ(j["tags"][0], std::string("alpha"));
  ASSERT_EQ(j["tags"][1], std::string("beta"));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_file_tags_replaced_emits_on_update_tags passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// REGRESSION GUARD: vxcore_file_tag on a nonexistent file returns
// VXCORE_ERR_NOT_FOUND BEFORE TagFile reaches SaveFolderConfig. The
// kFileTagged subscriber must observe zero emits.
// -------------------------------------------------------------------------
int test_no_emit_when_file_not_found() {
  std::cout << "  Running test_no_emit_when_file_not_found..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("md_evt_tag_nofile_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"TagNoFileTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  err = vxcore_tag_create(ctx, notebook_id, "stray");
  ASSERT_EQ(err, VXCORE_OK);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFileTagged, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "missing.md", "stray");
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  ASSERT_EQ(cap.call_count, 0);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_no_emit_when_file_not_found passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// REGRESSION GUARD: vxcore_file_tag with a tag NOT defined on the notebook
// returns VXCORE_ERR_INVALID_PARAM from notebook_->FindTag(tag_name) BEFORE
// TagFile reaches SaveFolderConfig. The kFileTagged subscriber must observe
// zero emits.
// -------------------------------------------------------------------------
int test_no_emit_when_tag_not_defined() {
  std::cout << "  Running test_no_emit_when_tag_not_defined..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  char *file_id = nullptr;
  std::string nb_path = make_notebook_with_file(ctx, "md_evt_tag_undef_nb", "note.md",
                                                &notebook_id, &file_id, &err);
  ASSERT_EQ(err, VXCORE_OK);

  // Intentionally DO NOT define the tag — FindTag will return nullptr and
  // TagFile must early-return INVALID_PARAM.
  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFileTagged, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_tag(ctx, notebook_id, "note.md", "undefined_tag");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  ASSERT_EQ(cap.call_count, 0);

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_no_emit_when_tag_not_defined passed" << std::endl;
  return 0;
}

// =========================================================================
// T7 subtests - Semantic attachment events.
//
// Owns coverage for kFileAttached / kFileDetached / kFileAttachmentsReplaced.
// Each emit must fire EXACTLY ONCE on the success path AFTER both
// SaveFolderConfig (disk write) AND MetadataStore write-through succeed.
// Idempotent / failure paths MUST NOT emit - the dedup early-return at
// bundled_folder_manager.cpp:2740 (AddFileAttachment) and the not-found
// early-return at :2782 (DeleteFileAttachment) PRECEDE the emit site.
//
// Reuses make_notebook_with_file declared in T5's anonymous namespace.
// =========================================================================

// -------------------------------------------------------------------------
// T7 POSITIVE: vxcore_file_add_attachment -> AddFileAttachment success path
// fires file.attached exactly once with payload
// {notebookId, path, attachment}.
// -------------------------------------------------------------------------
int test_file_attached_emits_on_add_attachment() {
  std::cout << "  Running test_file_attached_emits_on_add_attachment..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  char *file_id = nullptr;
  std::string nb_path = make_notebook_with_file(ctx, "md_evt_attach_ok_nb", "note.md",
                                                &notebook_id, &file_id, &err);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);
  ASSERT_NOT_NULL(file_id);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFileAttached, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_add_attachment(ctx, notebook_id, "note.md", "image.png");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT_EQ(cap.call_count, 1);
  ASSERT_EQ(cap.events[0], std::string(vxcore::events::kFileAttached));

  auto j = nlohmann::json::parse(cap.payloads[0]);
  ASSERT_TRUE(j.is_object());
  ASSERT_EQ(j["notebookId"], std::string(notebook_id));
  ASSERT_EQ(j["path"], std::string("note.md"));
  ASSERT_EQ(j["attachment"], std::string("image.png"));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  + test_file_attached_emits_on_add_attachment passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// T7 IDEMPOTENT GUARD: AddFileAttachment with attachment already in list
// returns VXCORE_OK via the dedup early-return at
// bundled_folder_manager.cpp:2740 and MUST NOT emit file.attached.
// -------------------------------------------------------------------------
int test_file_attached_no_emit_when_already_present() {
  std::cout << "  Running test_file_attached_no_emit_when_already_present..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  char *file_id = nullptr;
  std::string nb_path = make_notebook_with_file(ctx, "md_evt_attach_dup_nb", "note.md",
                                                &notebook_id, &file_id, &err);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_add_attachment(ctx, notebook_id, "note.md", "doc.pdf");
  ASSERT_EQ(err, VXCORE_OK);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFileAttached, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_add_attachment(ctx, notebook_id, "note.md", "doc.pdf");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT_EQ(cap.call_count, 0);

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  + test_file_attached_no_emit_when_already_present passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// T7 POSITIVE: vxcore_file_delete_attachment -> DeleteFileAttachment success
// path fires file.detached exactly once with payload
// {notebookId, path, attachment}.
// -------------------------------------------------------------------------
int test_file_detached_emits_on_delete_attachment() {
  std::cout << "  Running test_file_detached_emits_on_delete_attachment..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  char *file_id = nullptr;
  std::string nb_path = make_notebook_with_file(ctx, "md_evt_detach_ok_nb", "note.md",
                                                &notebook_id, &file_id, &err);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_add_attachment(ctx, notebook_id, "note.md", "data.zip");
  ASSERT_EQ(err, VXCORE_OK);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFileDetached, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_delete_attachment(ctx, notebook_id, "note.md", "data.zip");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT_EQ(cap.call_count, 1);
  ASSERT_EQ(cap.events[0], std::string(vxcore::events::kFileDetached));

  auto j = nlohmann::json::parse(cap.payloads[0]);
  ASSERT_TRUE(j.is_object());
  ASSERT_EQ(j["notebookId"], std::string(notebook_id));
  ASSERT_EQ(j["path"], std::string("note.md"));
  ASSERT_EQ(j["attachment"], std::string("data.zip"));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  + test_file_detached_emits_on_delete_attachment passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// T7 IDEMPOTENT GUARD: DeleteFileAttachment for attachment not in list
// returns VXCORE_OK via the not-found early-return at
// bundled_folder_manager.cpp:2782 and MUST NOT emit file.detached.
// -------------------------------------------------------------------------
int test_file_detached_no_emit_when_not_present() {
  std::cout << "  Running test_file_detached_no_emit_when_not_present..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  char *file_id = nullptr;
  std::string nb_path = make_notebook_with_file(ctx, "md_evt_detach_miss_nb", "note.md",
                                                &notebook_id, &file_id, &err);
  ASSERT_EQ(err, VXCORE_OK);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFileDetached, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_file_delete_attachment(ctx, notebook_id, "note.md", "nonexistent.pdf");
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT_EQ(cap.call_count, 0);

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  + test_file_detached_no_emit_when_not_present passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// T7 POSITIVE: vxcore_file_update_attachments -> UpdateFileAttachments
// success path fires file.attachments_replaced exactly once with payload
// {notebookId, path, attachments} where `attachments` is the EXACT JSON
// array of the new attachment list.
// -------------------------------------------------------------------------
int test_file_attachments_replaced_emits_on_update() {
  std::cout << "  Running test_file_attachments_replaced_emits_on_update..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  char *file_id = nullptr;
  std::string nb_path = make_notebook_with_file(ctx, "md_evt_replace_attach_nb", "note.md",
                                                &notebook_id, &file_id, &err);
  ASSERT_EQ(err, VXCORE_OK);

  EventCapture cap;
  err = vxcore_on_event(ctx, vxcore::events::kFileAttachmentsReplaced, capture_event_cb, &cap);
  ASSERT_EQ(err, VXCORE_OK);

  const char *attachments_payload = "[\"img.png\",\"doc.pdf\"]";
  err = vxcore_file_update_attachments(ctx, notebook_id, "note.md", attachments_payload);
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT_EQ(cap.call_count, 1);
  ASSERT_EQ(cap.events[0], std::string(vxcore::events::kFileAttachmentsReplaced));

  auto j = nlohmann::json::parse(cap.payloads[0]);
  ASSERT_TRUE(j.is_object());
  ASSERT_EQ(j["notebookId"], std::string(notebook_id));
  ASSERT_EQ(j["path"], std::string("note.md"));
  ASSERT_TRUE(j["attachments"].is_array());
  ASSERT_EQ(j["attachments"].size(), static_cast<size_t>(2));
  ASSERT_EQ(j["attachments"][0], std::string("img.png"));
  ASSERT_EQ(j["attachments"][1], std::string("doc.pdf"));

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  + test_file_attachments_replaced_emits_on_update passed" << std::endl;
  return 0;
}

// -------------------------------------------------------------------------
// T13 REGRESSION GUARD: vxcore_notebook_create does NOT emit metadata events
// for the initial root config writes (UpdateConfig called in
// BundledNotebook::InitOnCreation, SaveFolderConfig for root folder setup).
//
// Root cause: event_manager is wired AFTER BundledNotebook::Create completes
// per notebook_manager.cpp:124-125, so creation-time writes silently drop
// their emits (FolderManager::EmitEvent checks event_manager_ != nullptr).
//
// This test locks the contract: creation-time silence is EXPECTED and
// INTENTIONAL. Accidental earlier wiring (e.g., moving SetEventManager to
// line 122) would cause unwanted sync.should_run events before the notebook
// is in configs_cache_, creating subtle bugs.
//
// Strategy:
// 1. Create context + EventManager + SyncManager (full setup)
// 2. Subscribe to TWO event types: kNotebookConfigChanged and kFolderConfigChanged
// 3. Call vxcore_notebook_create (should emit ZERO for both)
// 4. Sanity check: vxcore_folder_create on the new notebook (should emit >=1
//    for kFolderConfigChanged, proving the EventManager is functional)
// -------------------------------------------------------------------------
int test_create_notebook_no_emit_during_creation() {
  std::cout << "  Running test_create_notebook_no_emit_during_creation..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Subscribe to notebook config events (counter A).
  EventCapture cap_notebook;
  err = vxcore_on_event(ctx, vxcore::events::kNotebookConfigChanged, capture_event_cb,
                        &cap_notebook);
  ASSERT_EQ(err, VXCORE_OK);

  // Subscribe to folder config events (counter B).
  EventCapture cap_folder;
  err = vxcore_on_event(ctx, vxcore::events::kFolderConfigChanged, capture_event_cb,
                        &cap_folder);
  ASSERT_EQ(err, VXCORE_OK);

  // Create notebook. The internal UpdateConfig (InitOnCreation) and root
  // SaveFolderConfig calls should NOT emit because event_manager is nullptr
  // during BundledNotebook::Create.
  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("md_evt_create_no_emit_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(),
                               "{\"name\":\"CreateNoEmitTest\",\"description\":\"Testing\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Assert: creation-time silence (A == 0 AND B == 0).
  // This locks the contract that event_manager wiring happens AFTER
  // BundledNotebook::Create.
  ASSERT_EQ(cap_notebook.call_count, 0);
  ASSERT_EQ(cap_folder.call_count, 0);

  // Sanity check: now that the notebook is wired, a subsequent operation
  // MUST emit. Create a folder on the new notebook.
  char *folder_id = nullptr;
  int cap_folder_before = cap_folder.call_count;
  err = vxcore_folder_create(ctx, notebook_id, ".", "SanityCheckFolder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(folder_id);

  // The folder creation MUST have fired at least one kFolderConfigChanged emit
  // for the root folder update (parent). This proves the EventManager is
  // functional and was correctly wired AFTER the initial creation.
  int cap_folder_after = cap_folder.call_count;
  ASSERT_TRUE(cap_folder_after > cap_folder_before);

  // Cleanup.
  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_create_notebook_no_emit_during_creation passed (A=" << cap_notebook.call_count
            << " B=" << cap_folder.call_count << " B_sanity=" << cap_folder_after << ")" << std::endl;
  return 0;
}

int main() {
  // T4 subtests (folder.config_changed persistence event).
  RUN_TEST(test_folder_config_changed_emits_on_create_folder);
  RUN_TEST(test_folder_config_changed_no_emit_on_failure);

  // T5 subtests (file.tagged / file.untagged / file.tags_replaced semantic
  // events). Six subtests: 3 positive emit cases + 1 idempotent UntagFile
  // guard + 2 failure-path guards (file-not-found, tag-not-defined).
  RUN_TEST(test_file_tagged_emits_on_tag_file);
  RUN_TEST(test_file_untagged_emits_on_untag_file);
  RUN_TEST(test_file_untagged_no_emit_when_tag_not_present);
  RUN_TEST(test_file_tags_replaced_emits_on_update_tags);
  RUN_TEST(test_no_emit_when_file_not_found);
  RUN_TEST(test_no_emit_when_tag_not_defined);

  // T6 subtests (file.metadata_updated / folder.metadata_updated semantic events).
  RUN_TEST(test_file_metadata_updated_emits_on_update_file_metadata);
  RUN_TEST(test_folder_metadata_updated_emits_on_update_folder_metadata);
  RUN_TEST(test_no_emit_on_invalid_metadata_json);
  RUN_TEST(test_no_emit_on_nonexistent_file_metadata);

  // T8 subtests (notebook.config_changed persistence event).
  RUN_TEST(test_notebook_config_changed_emits_on_update_config);
  RUN_TEST(test_notebook_config_changed_no_emit_when_event_manager_absent);
  RUN_TEST(test_notebook_config_changed_no_emit_on_write_failure);

  // T13 REGRESSION GUARD: CreateNotebook creation-time no emit.
  RUN_TEST(test_create_notebook_no_emit_during_creation);

  // T7 subtests (file.attached / file.detached / file.attachments_replaced
  // semantic attachment events). Three positive emits + two idempotent
  // guards covering the dedup early-return (add) and not-found early-return
  // (delete).
  RUN_TEST(test_file_attached_emits_on_add_attachment);
  RUN_TEST(test_file_attached_no_emit_when_already_present);
  RUN_TEST(test_file_detached_emits_on_delete_attachment);
  RUN_TEST(test_file_detached_no_emit_when_not_present);
  RUN_TEST(test_file_attachments_replaced_emits_on_update);

  std::cout << "All metadata event tests passed!" << std::endl;
  return 0;
}
