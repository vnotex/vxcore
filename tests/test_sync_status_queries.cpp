// Task 7.4 (F3.6): coverage for SyncManager::IsReady + LastSyncTime and the
// matching C API delegations (vxcore_sync_is_ready,
// vxcore_sync_get_last_sync_utc). Verifies the two helpers route through the
// single source of truth on SyncManager and that the Sync code path updates
// the per-device last-success timestamp.

#include "test_utils.h"
#include "vxcore/vxcore.h"

#include <chrono>
#include <string>

#include "core/context.h"
#include "core/notebook.h"
#include "core/notebook_manager.h"
#include "sync/sync_manager.h"
#include "test_internals/mock_sync_backend.h"

// Force-link the test_internals TU so its anonymous-namespace
// BackendRegistration token runs at static-init time and "mock" lands in
// SyncBackendRegistry::Instance(). Mirrors the pattern from
// test_event_manager.cpp.
namespace {
[[maybe_unused]] vxcore::MockSyncBackend *g_force_link_test_internals_mock =
    new vxcore::MockSyncBackend();
}  // namespace

namespace {

// Persist the notebook's sync_* config fields so SyncManager::IsReady (which
// reads NotebookConfig) returns true. vxcore_sync_enable only populates the
// runtime states_/backends_ maps — persisted JSON fields are normally written
// by the Qt SyncService layer. Returns VxCoreError so the caller can ASSERT.
VxCoreError PersistSyncFields(VxCoreContextHandle ctx, const char *notebook_id,
                              const std::string &backend, const std::string &remote_url) {
  std::string json = "{\"syncEnabled\":true,\"syncBackend\":\"" + backend +
                     "\",\"syncRemoteUrl\":\"" + remote_url + "\"}";
  return vxcore_notebook_update_config(ctx, notebook_id, json.c_str());
}

// Clear the persisted sync fields (mirrors what SyncService writes on disable).
VxCoreError ClearPersistedSyncFields(VxCoreContextHandle ctx, const char *notebook_id) {
  const char *json = "{\"syncEnabled\":false,\"syncBackend\":\"\",\"syncRemoteUrl\":\"\"}";
  return vxcore_notebook_update_config(ctx, notebook_id, json);
}

// Local copy of vxcore::GetCurrentTimestampMillis — that helper isn't exported
// from the DLL (no VXCORE_API), so per libs/vxcore/AGENTS.md the test must
// re-derive it locally rather than try to link the internal symbol.
int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

int test_is_ready_after_enable() {
  std::cout << "  Running test_is_ready_after_enable..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("ready_after_enable_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"ReadyAfterEnable\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Before any sync setup: not ready.
  int ready = -1;
  err = vxcore_sync_is_ready(ctx, notebook_id, &ready);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(ready, 0);

  // EnableSync via C API populates runtime state, but IsReady's predicate
  // reads the persisted notebook config — emulate what SyncService writes.
  err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"mock\"}", nullptr);
  ASSERT_EQ(err, VXCORE_OK);
  err = PersistSyncFields(ctx, notebook_id, "mock", "mock://test");
  ASSERT_EQ(err, VXCORE_OK);

  // Now the C API reports ready.
  ready = -1;
  err = vxcore_sync_is_ready(ctx, notebook_id, &ready);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(ready, 1);

  // And the SyncManager::IsReady direct call agrees.
  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ASSERT_TRUE(vctx->sync_manager->IsReady(notebook_id));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_is_ready_after_enable passed" << std::endl;
  return 0;
}

int test_is_ready_false_after_disable() {
  std::cout << "  Running test_is_ready_false_after_disable..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("ready_after_disable_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"ReadyAfterDisable\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"mock\"}", nullptr);
  ASSERT_EQ(err, VXCORE_OK);
  err = PersistSyncFields(ctx, notebook_id, "mock", "mock://test");
  ASSERT_EQ(err, VXCORE_OK);

  int ready = -1;
  err = vxcore_sync_is_ready(ctx, notebook_id, &ready);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(ready, 1);

  // Disable: vxcore_sync_disable clears runtime state; the Qt layer clears
  // the persisted fields. Replicate both for an accurate disable simulation.
  err = vxcore_sync_disable(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  err = ClearPersistedSyncFields(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  ready = -1;
  err = vxcore_sync_is_ready(ctx, notebook_id, &ready);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(ready, 0);

  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ASSERT_FALSE(vctx->sync_manager->IsReady(notebook_id));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_is_ready_false_after_disable passed" << std::endl;
  return 0;
}

int test_last_sync_updated_on_success() {
  std::cout << "  Running test_last_sync_updated_on_success..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("last_sync_nb");
  cleanup_test_dir(nb_path);
  err = vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"LastSyncSuccess\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Before any sync: last-sync-time is 0.
  int64_t ts = -1;
  err = vxcore_sync_get_last_sync_utc(ctx, notebook_id, &ts);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(ts, 0);

  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ASSERT_EQ(vctx->sync_manager->LastSyncTime(notebook_id), 0);

  err = vxcore_sync_enable(ctx, notebook_id, "{\"backend\":\"mock\"}", nullptr);
  ASSERT_EQ(err, VXCORE_OK);

  const int64_t before = NowMillis();

  // Mock backend's Sync() returns VXCORE_OK by default, which triggers
  // SyncManager::TriggerSync to write Notebook::SetLastSyncUtc(now).
  err = vxcore_sync_trigger(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  const int64_t after = NowMillis();

  ts = -1;
  err = vxcore_sync_get_last_sync_utc(ctx, notebook_id, &ts);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_TRUE(ts >= before);
  ASSERT_TRUE(ts <= after);

  // SyncManager::LastSyncTime returns the same value.
  ASSERT_EQ(vctx->sync_manager->LastSyncTime(notebook_id), ts);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  ✓ test_last_sync_updated_on_success passed" << std::endl;
  return 0;
}

int main() {
  RUN_TEST(test_is_ready_after_enable);
  RUN_TEST(test_is_ready_false_after_disable);
  RUN_TEST(test_last_sync_updated_on_success);
  std::cout << "\nAll test_sync_status_queries tests passed!" << std::endl;
  return 0;
}
