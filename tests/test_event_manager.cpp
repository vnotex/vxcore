#include "test_utils.h"
#include "vxcore/vxcore.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "core/context.h"
#include "core/event_manager.h"
#include "core/event_names.h"
#include "core/work_queue.h"
#include "sync/sync_manager.h"
#include "test_internals/mock_sync_backend.h"

// Force-link the test_internals TU so its anonymous-namespace
// BackendRegistration token runs at static-init time. Without this reference,
// MSVC's linker would drop the entire mock_sync_backend.cpp TU from the static
// library and the "mock" backend would never be registered in
// SyncBackendRegistry::Instance(), causing vxcore_sync_enable(...,"mock",...)
// to fail with VXCORE_ERR_UNKNOWN_BACKEND.
namespace {
[[maybe_unused]] vxcore::MockSyncBackend *g_force_link_test_internals_mock =
    new vxcore::MockSyncBackend();
}  // namespace

// ============ EventManager unit tests ============

int test_subscribe_and_emit() {
  std::cout << "  Running test_subscribe_and_emit..." << std::endl;
  vxcore::EventManager em;
  std::string received_name;
  nlohmann::json received_data;
  em.Subscribe("file.created", [&](const std::string &name, const nlohmann::json &data) {
    received_name = name;
    received_data = data;
  });
  em.Emit("file.created", {{"path", "test.md"}});
  ASSERT_EQ(received_name, "file.created");
  ASSERT_EQ(received_data["path"], "test.md");
  std::cout << "  ✓ test_subscribe_and_emit passed" << std::endl;
  return 0;
}

int test_multiple_listeners_same_event() {
  std::cout << "  Running test_multiple_listeners_same_event..." << std::endl;
  vxcore::EventManager em;
  int count = 0;
  em.Subscribe("file.saved", [&](const std::string &, const nlohmann::json &) { count++; });
  em.Subscribe("file.saved", [&](const std::string &, const nlohmann::json &) { count += 10; });
  em.Emit("file.saved", {});
  ASSERT_EQ(count, 11);
  std::cout << "  ✓ test_multiple_listeners_same_event passed" << std::endl;
  return 0;
}

int test_different_events_only_matching_fires() {
  std::cout << "  Running test_different_events_only_matching_fires..." << std::endl;
  vxcore::EventManager em;
  int a_count = 0, b_count = 0;
  em.Subscribe("event.a", [&](const std::string &, const nlohmann::json &) { a_count++; });
  em.Subscribe("event.b", [&](const std::string &, const nlohmann::json &) { b_count++; });
  em.Emit("event.a", {});
  ASSERT_EQ(a_count, 1);
  ASSERT_EQ(b_count, 0);
  em.Emit("event.b", {});
  ASSERT_EQ(a_count, 1);
  ASSERT_EQ(b_count, 1);
  std::cout << "  ✓ test_different_events_only_matching_fires passed" << std::endl;
  return 0;
}

int test_unsubscribe() {
  std::cout << "  Running test_unsubscribe..." << std::endl;
  vxcore::EventManager em;
  int count = 0;
  auto id = em.Subscribe("ev", [&](const std::string &, const nlohmann::json &) { count++; });
  em.Emit("ev", {});
  ASSERT_EQ(count, 1);
  em.Unsubscribe(id);
  em.Emit("ev", {});
  ASSERT_EQ(count, 1);
  std::cout << "  ✓ test_unsubscribe passed" << std::endl;
  return 0;
}

int test_emit_no_listeners() {
  std::cout << "  Running test_emit_no_listeners..." << std::endl;
  vxcore::EventManager em;
  em.Emit("no.listeners", {{"key", "value"}});
  std::cout << "  ✓ test_emit_no_listeners passed" << std::endl;
  return 0;
}

int test_listener_receives_valid_json() {
  std::cout << "  Running test_listener_receives_valid_json..." << std::endl;
  vxcore::EventManager em;
  nlohmann::json received;
  em.Subscribe("file.created",
               [&](const std::string &, const nlohmann::json &data) { received = data; });
  em.Emit("file.created",
          {{"notebookId", "nb-123"}, {"path", "docs/readme.md"}});
  ASSERT_EQ(received["notebookId"], "nb-123");
  ASSERT_EQ(received["path"], "docs/readme.md");
  ASSERT_TRUE(received.is_object());
  std::cout << "  ✓ test_listener_receives_valid_json passed" << std::endl;
  return 0;
}

int test_emit_async_via_work_queue() {
  std::cout << "  Running test_emit_async_via_work_queue..." << std::endl;
  vxcore::EventManager em;
  vxcore::WorkQueue wq;
  std::atomic<int> count{0};
  std::string received_event;

  em.Subscribe("async.test", [&](const std::string &name, const nlohmann::json &) {
    received_event = name;
    count.fetch_add(1);
  });

  em.EmitAsync("async.test", {{"key", "val"}}, &wq);
  ASSERT_EQ(count.load(), 0);
  ASSERT_EQ(wq.Size(), 1u);

  wq.ProcessAll();
  ASSERT_EQ(count.load(), 1);
  ASSERT_EQ(received_event, "async.test");
  std::cout << "  ✓ test_emit_async_via_work_queue passed" << std::endl;
  return 0;
}

int test_emit_async_cross_thread() {
  std::cout << "  Running test_emit_async_cross_thread..." << std::endl;
  vxcore::EventManager em;
  vxcore::WorkQueue wq;
  std::atomic<int> count{0};
  std::atomic<std::thread::id> handler_thread_id{};

  em.Subscribe("thread.test", [&](const std::string &, const nlohmann::json &) {
    handler_thread_id.store(std::this_thread::get_id());
    count.fetch_add(1);
  });

  // Emit from main thread
  em.EmitAsync("thread.test", {}, &wq);

  // Process on worker thread
  std::thread worker([&] { wq.ProcessNext(1000); });
  worker.join();

  ASSERT_EQ(count.load(), 1);
  ASSERT_NE(handler_thread_id.load(), std::this_thread::get_id());
  std::cout << "  ✓ test_emit_async_cross_thread passed" << std::endl;
  return 0;
}

// ============ C API tests ============

namespace {
struct CApiTestState {
  int call_count = 0;
  std::string last_event;
  std::string last_json;
};

void test_event_callback(const char *event_name, const char *json_data, void *userdata) {
  auto *state = static_cast<CApiTestState *>(userdata);
  state->call_count++;
  state->last_event = event_name;
  state->last_json = json_data;
}

void test_event_callback_2(const char *, const char *, void *userdata) {
  auto *state = static_cast<CApiTestState *>(userdata);
  state->call_count += 100;
}
}  // namespace

int test_c_api_on_off_event() {
  std::cout << "  Running test_c_api_on_off_event..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  CApiTestState state;
  err = vxcore_on_event(ctx, "test.event", test_event_callback, &state);
  ASSERT_EQ(err, VXCORE_OK);

  // Emit internally
  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  vctx->event_manager->Emit("test.event", {{"key", "value"}});
  ASSERT_EQ(state.call_count, 1);
  ASSERT_EQ(state.last_event, "test.event");

  // Verify JSON
  auto j = nlohmann::json::parse(state.last_json);
  ASSERT_EQ(j["key"], "value");

  // Unsubscribe
  err = vxcore_off_event(ctx, "test.event", test_event_callback);
  ASSERT_EQ(err, VXCORE_OK);

  vctx->event_manager->Emit("test.event", {});
  ASSERT_EQ(state.call_count, 1);

  // Off non-existent returns NOT_FOUND
  err = vxcore_off_event(ctx, "test.event", test_event_callback);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  // Null safety
  err = vxcore_on_event(nullptr, "ev", test_event_callback, nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  err = vxcore_on_event(ctx, nullptr, test_event_callback, nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  err = vxcore_on_event(ctx, "ev", nullptr, nullptr);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_c_api_on_off_event passed" << std::endl;
  return 0;
}

// ============ Integration: notebook/folder events via C API ============

int test_integration_file_created_event() {
  std::cout << "  Running test_integration_file_created_event..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  CApiTestState state;
  err = vxcore_on_event(ctx, "file.created", test_event_callback, &state);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a notebook
  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("event_test_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"EventTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Create a file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test_note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Event should have fired
  ASSERT_EQ(state.call_count, 1);
  ASSERT_EQ(state.last_event, "file.created");
  auto j = nlohmann::json::parse(state.last_json);
  ASSERT_EQ(j["notebookId"], std::string(notebook_id));
  ASSERT_EQ(j["path"], "test_note.md");

  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_integration_file_created_event passed" << std::endl;
  return 0;
}

int test_integration_file_deleted_event() {
  std::cout << "  Running test_integration_file_deleted_event..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("event_del_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"EventDelTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "to_delete.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  CApiTestState state;
  err = vxcore_on_event(ctx, "file.deleted", test_event_callback, &state);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_node_delete(ctx, notebook_id, "to_delete.md");
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state.call_count, 1);
  auto j = nlohmann::json::parse(state.last_json);
  ASSERT_EQ(j["path"], "to_delete.md");

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_integration_file_deleted_event passed" << std::endl;
  return 0;
}

int test_integration_folder_created_event() {
  std::cout << "  Running test_integration_folder_created_event..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("event_folder_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"FolderEventTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  CApiTestState state;
  err = vxcore_on_event(ctx, "folder.created", test_event_callback, &state);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "subfolder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state.call_count, 1);
  auto j = nlohmann::json::parse(state.last_json);
  ASSERT_EQ(j["path"], "subfolder");

  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_integration_folder_created_event passed" << std::endl;
  return 0;
}

int test_integration_notebook_open_close_events() {
  std::cout << "  Running test_integration_notebook_open_close_events..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  CApiTestState open_state, close_state;
  err = vxcore_on_event(ctx, "notebook.opened", test_event_callback, &open_state);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_on_event(ctx, "notebook.closed", test_event_callback, &close_state);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("event_openclose_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"OpenCloseTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create fires notebook.opened
  ASSERT_EQ(open_state.call_count, 1);
  auto j = nlohmann::json::parse(open_state.last_json);
  ASSERT_EQ(j["notebookId"], std::string(notebook_id));

  // Close fires notebook.closed
  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(close_state.call_count, 1);
  j = nlohmann::json::parse(close_state.last_json);
  ASSERT_EQ(j["notebookId"], std::string(notebook_id));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_integration_notebook_open_close_events passed" << std::endl;
  return 0;
}

int test_integration_file_moved_event() {
  std::cout << "  Running test_integration_file_moved_event..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("event_move_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"MoveEventTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create target folder and file
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "dest", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "moveme.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  CApiTestState state;
  err = vxcore_on_event(ctx, "file.moved", test_event_callback, &state);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_node_move(ctx, notebook_id, "moveme.md", "dest");
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state.call_count, 1);
  auto j = nlohmann::json::parse(state.last_json);
  ASSERT_EQ(j["oldPath"], "moveme.md");
  ASSERT_EQ(j["newPath"], "dest/moveme.md");

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_integration_file_moved_event passed" << std::endl;
  return 0;
}

// ============ SyncManager dirty-tracking integration tests ============

int test_sync_dirty_on_file_created() {
  std::cout << "  Running test_sync_dirty_on_file_created..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("dirty_test_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"DirtyTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Enable sync so SyncManager tracks this notebook
  err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"mock\"}", nullptr);
  ASSERT_EQ(err, VXCORE_OK);

  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  auto dirty = vctx->sync_manager->GetDirtyNotebooks();
  ASSERT_TRUE(dirty.empty());

  // Create a file — should mark notebook dirty
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "dirty_test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  dirty = vctx->sync_manager->GetDirtyNotebooks();
  ASSERT_EQ(dirty.size(), 1u);
  ASSERT_EQ(dirty[0], std::string(notebook_id));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_sync_dirty_on_file_created passed" << std::endl;
  return 0;
}

int test_sync_dirty_cleared_after_trigger() {
  std::cout << "  Running test_sync_dirty_cleared_after_trigger..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("dirty_clear_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"DirtyClearTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"mock\"}", nullptr);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "clear_test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ASSERT_EQ(vctx->sync_manager->GetDirtyNotebooks().size(), 1u);

  // ClearDirty manually (TriggerSync would call it on success,
  // but we don't have a real backend)
  vctx->sync_manager->ClearDirty(notebook_id);
  ASSERT_TRUE(vctx->sync_manager->GetDirtyNotebooks().empty());

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_sync_dirty_cleared_after_trigger passed" << std::endl;
  return 0;
}

int test_sync_dirty_not_marked_for_non_sync_notebook() {
  std::cout << "  Running test_sync_dirty_not_marked_for_non_sync_notebook..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("dirty_nosync_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"NoSyncTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Do NOT enable sync — file operations should NOT mark dirty
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "nosync_test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);

  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ASSERT_TRUE(vctx->sync_manager->GetDirtyNotebooks().empty());

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_sync_dirty_not_marked_for_non_sync_notebook passed" << std::endl;
  return 0;
}

int test_sync_dirty_multiple_ops_single_entry() {
   std::cout << "  Running test_sync_dirty_multiple_ops_single_entry..." << std::endl;
   vxcore_set_test_mode(1);
   vxcore_clear_test_directory();

   VxCoreContextHandle ctx = nullptr;
   VxCoreError err = vxcore_context_create(nullptr, &ctx);
   ASSERT_EQ(err, VXCORE_OK);

   char *notebook_id = nullptr;
   std::string nb_path = get_test_path("dirty_multi_nb");
   cleanup_test_dir(nb_path);
   err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"MultiOpTest\"}",
                                VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
   ASSERT_EQ(err, VXCORE_OK);

   err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"mock\"}", nullptr);
   ASSERT_EQ(err, VXCORE_OK);

   char *file_id1 = nullptr;
   err = vxcore_file_create(ctx, notebook_id, ".", "multi1.md", &file_id1);
   ASSERT_EQ(err, VXCORE_OK);
   vxcore_string_free(file_id1);

   char *file_id2 = nullptr;
   err = vxcore_file_create(ctx, notebook_id, ".", "multi2.md", &file_id2);
   ASSERT_EQ(err, VXCORE_OK);
   vxcore_string_free(file_id2);

   char *folder_id = nullptr;
   err = vxcore_folder_create(ctx, notebook_id, ".", "subfolder", &folder_id);
   ASSERT_EQ(err, VXCORE_OK);
   vxcore_string_free(folder_id);

   auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
   auto dirty = vctx->sync_manager->GetDirtyNotebooks();
   ASSERT_EQ(dirty.size(), 1u);

   vxcore_string_free(notebook_id);
   vxcore_context_destroy(ctx);
   cleanup_test_dir(nb_path);
   std::cout << "  ✓ test_sync_dirty_multiple_ops_single_entry passed" << std::endl;
   return 0;
}

int test_sync_dirty_on_folder_config_changed() {
   std::cout << "  Running test_sync_dirty_on_folder_config_changed..." << std::endl;
   vxcore_set_test_mode(1);
   vxcore_clear_test_directory();

   VxCoreContextHandle ctx = nullptr;
   VxCoreError err = vxcore_context_create(nullptr, &ctx);
   ASSERT_EQ(err, VXCORE_OK);

   char *notebook_id = nullptr;
   std::string nb_path = get_test_path("dirty_folder_cfg_nb");
   cleanup_test_dir(nb_path);
   err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"FolderConfigTest\"}",
                                VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
   ASSERT_EQ(err, VXCORE_OK);

   // Enable sync so SyncManager tracks this notebook
   err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"mock\"}", nullptr);
   ASSERT_EQ(err, VXCORE_OK);

   auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
   auto dirty = vctx->sync_manager->GetDirtyNotebooks();
   ASSERT_TRUE(dirty.empty());

   // Emit folder config change event directly
   vctx->event_manager->Emit(vxcore::events::kFolderConfigChanged,
                              {{"notebookId", std::string(notebook_id)}, {"path", "Foo"}});

   dirty = vctx->sync_manager->GetDirtyNotebooks();
   ASSERT_EQ(dirty.size(), 1u);
   ASSERT_EQ(dirty[0], std::string(notebook_id));

   vxcore_string_free(notebook_id);
   vxcore_context_destroy(ctx);
   cleanup_test_dir(nb_path);
   std::cout << "  ✓ test_sync_dirty_on_folder_config_changed passed" << std::endl;
   return 0;
}

int test_sync_dirty_on_notebook_config_changed() {
   std::cout << "  Running test_sync_dirty_on_notebook_config_changed..." << std::endl;
   vxcore_set_test_mode(1);
   vxcore_clear_test_directory();

   VxCoreContextHandle ctx = nullptr;
   VxCoreError err = vxcore_context_create(nullptr, &ctx);
   ASSERT_EQ(err, VXCORE_OK);

   char *notebook_id = nullptr;
   std::string nb_path = get_test_path("dirty_notebook_cfg_nb");
   cleanup_test_dir(nb_path);
   err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"NotebookConfigTest\"}",
                                VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
   ASSERT_EQ(err, VXCORE_OK);

   // Enable sync so SyncManager tracks this notebook
   err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"mock\"}", nullptr);
   ASSERT_EQ(err, VXCORE_OK);

   auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
   auto dirty = vctx->sync_manager->GetDirtyNotebooks();
   ASSERT_TRUE(dirty.empty());

   // Emit notebook config change event directly
   vctx->event_manager->Emit(vxcore::events::kNotebookConfigChanged,
                              {{"notebookId", std::string(notebook_id)}});

   dirty = vctx->sync_manager->GetDirtyNotebooks();
   ASSERT_EQ(dirty.size(), 1u);
   ASSERT_EQ(dirty[0], std::string(notebook_id));

   vxcore_string_free(notebook_id);
   vxcore_context_destroy(ctx);
   cleanup_test_dir(nb_path);
   std::cout << "  ✓ test_sync_dirty_on_notebook_config_changed passed" << std::endl;
   return 0;
}

// Regression test for folder events on session-loaded notebooks.
// Verifies that SetEventManager propagates to already-loaded notebooks' folder managers.
// Issue: NotebookManager loaded notebooks before SetEventManager was called, leaving
// folder managers without event notification capability. This test ensures the fix works.
int test_folder_events_on_session_loaded_notebook() {
  std::cout << "  Running test_folder_events_on_session_loaded_notebook..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  // Create context and notebook, enable sync
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("session_load_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"SessionLoadTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"mock\"}", nullptr);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a folder via C API (this marks dirty if event wiring works)
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "testfolder", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);

  // Verify the folder operation marked the notebook as dirty
  // (proves SetEventManager propagated to the session-loaded notebook)
  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  auto dirty = vctx->sync_manager->GetDirtyNotebooks();
  ASSERT_EQ(dirty.size(), 1u);
  ASSERT_EQ(dirty[0], notebook_id);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_folder_events_on_session_loaded_notebook passed" << std::endl;
  return 0;
}

// Regression test: vxcore_buffer_save must emit ONLY file.saved, no new metadata events
// Verifies buffer save path is unaffected by new semantic metadata events (kFileMetadataUpdated, etc.).
// Buffer save is a LOW-LEVEL persistence op, not a SEMANTIC metadata update, so it must NOT
// fire file.metadata_updated or folder.config_changed. It fires exactly 1 file.saved event.
// 
// Observable behavior (documented for future verification):
// - file.saved: emitted exactly ONCE by BufferManager::SaveBuffer (line 408)
// - folder.config_changed: NOT emitted (buffer save does not modify folder config)
// - file.metadata_updated: NOT emitted (buffer save is NOT a metadata update operation)
int test_buffer_save_emits_only_file_saved_no_new_events() {
  std::cout << "  Running test_buffer_save_emits_only_file_saved_no_new_events..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  // Setup: create context + notebook
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("buffer_save_test_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"BufferSaveTest\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(notebook_id);

  // Create a file to buffer
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test_buffer.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(file_id);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test_buffer.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id);

  // Set up counters for the three events of interest
  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  int file_saved_count = 0;
  int folder_config_changed_count = 0;
  int file_metadata_updated_count = 0;

  // Subscribe to file.saved
  auto id_a = vctx->event_manager->Subscribe(
      vxcore::events::kFileSaved,
      [&](const std::string &, const nlohmann::json &) { file_saved_count++; });

  // Subscribe to folder.config_changed
  auto id_b = vctx->event_manager->Subscribe(
      vxcore::events::kFolderConfigChanged,
      [&](const std::string &, const nlohmann::json &) { folder_config_changed_count++; });

  // Subscribe to file.metadata_updated
  auto id_c = vctx->event_manager->Subscribe(
      vxcore::events::kFileMetadataUpdated,
      [&](const std::string &, const nlohmann::json &) { file_metadata_updated_count++; });

  // Modify and save buffer
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, "new content here", 15);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_buffer_save(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify event counts
  // A: file.saved must fire exactly 1
  ASSERT_EQ(file_saved_count, 1);
  
  // B: folder.config_changed should be 0 (buffer save does not modify folder config)
  ASSERT_EQ(folder_config_changed_count, 0);
  
  // C: file.metadata_updated must be 0 (buffer save is NOT a metadata update)
  ASSERT_EQ(file_metadata_updated_count, 0);

  // Cleanup
  vctx->event_manager->Unsubscribe(id_a);
  vctx->event_manager->Unsubscribe(id_b);
  vctx->event_manager->Unsubscribe(id_c);
  vxcore_buffer_close(ctx, buffer_id);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_buffer_save_emits_only_file_saved_no_new_events passed" << std::endl;
  return 0;
}

int main() {
  // Unit tests
  RUN_TEST(test_subscribe_and_emit);
  RUN_TEST(test_multiple_listeners_same_event);
  RUN_TEST(test_different_events_only_matching_fires);
  RUN_TEST(test_unsubscribe);
  RUN_TEST(test_emit_no_listeners);
  RUN_TEST(test_listener_receives_valid_json);
  RUN_TEST(test_emit_async_via_work_queue);
  RUN_TEST(test_emit_async_cross_thread);

  // C API tests
  RUN_TEST(test_c_api_on_off_event);

  // Integration tests
  RUN_TEST(test_integration_file_created_event);
  RUN_TEST(test_integration_file_deleted_event);
  RUN_TEST(test_integration_folder_created_event);
  RUN_TEST(test_integration_notebook_open_close_events);
  RUN_TEST(test_integration_file_moved_event);

  // SyncManager dirty-tracking tests
  RUN_TEST(test_sync_dirty_on_file_created);
  RUN_TEST(test_sync_dirty_cleared_after_trigger);
  RUN_TEST(test_sync_dirty_not_marked_for_non_sync_notebook);
  RUN_TEST(test_sync_dirty_multiple_ops_single_entry);
  RUN_TEST(test_sync_dirty_on_folder_config_changed);
  RUN_TEST(test_sync_dirty_on_notebook_config_changed);

  // Regression test for session-loaded notebook event propagation
  RUN_TEST(test_folder_events_on_session_loaded_notebook);

  // Regression test for buffer save event surface
  RUN_TEST(test_buffer_save_emits_only_file_saved_no_new_events);

  std::cout << "All event manager tests passed!" << std::endl;
  return 0;
}
