// Regression tests for vxcore_sync_unregister_notebook /
// SyncManager::UnregisterBackend.
//
// Background: closing a notebook in VNote left the libgit2 git_repository*
// alive inside SyncManager::backends_ because the only existing path that
// destroyed it (SyncManager::DisableSync) ALSO wiped the on-disk JSON
// sync_* fields and the keychain PAT — wrong semantic for "user is
// closing the notebook, not disabling sync". The new
// vxcore_sync_unregister_notebook drops the runtime state without
// touching disk or credentials so the libgit2 pack-file mmaps are
// released immediately and Windows lets the user delete the notebook
// folder.
//
// These tests pin down the contract: idempotent on unknown ids,
// flips is_registered to 0, preserves disk JSON, supports
// re-registration, and validates null-pointer guards.
//
// Mirrors the test_vxcore_sync_is_registered.cpp template (mock backend,
// vxcore_test_internals link via INTERNALS, force-link trick).

#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "test_internals/mock_sync_backend.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"

// Force-link the mock-backend registration TU (mirrors test_sync.cpp and
// test_vxcore_sync_is_registered.cpp). Without this reference MSVC's
// linker drops mock_sync_backend.cpp and the "mock" backend never
// registers in SyncBackendRegistry, breaking
// vxcore_sync_enable(..., "mock", ...).
namespace {
[[maybe_unused]] vxcore::MockSyncBackend *g_force_link_test_internals_mock =
    new vxcore::MockSyncBackend();
}  // namespace

// Null-pointer guards: both context and notebook_id are required.
int test_unregister_null_pointers() {
  std::cout << "  Running test_unregister_null_pointers..." << std::endl;

  ASSERT_EQ(vxcore_sync_unregister_notebook(nullptr, "id"), VXCORE_ERR_NULL_POINTER);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  ASSERT_EQ(vxcore_sync_unregister_notebook(ctx, nullptr), VXCORE_ERR_NULL_POINTER);

  vxcore_context_destroy(ctx);
  std::cout << "  \xE2\x9C\x93 test_unregister_null_pointers passed" << std::endl;
  return 0;
}

// Unknown notebook: must succeed (idempotent). The Qt-side close path
// fires NotebookAfterClose for every notebook, including those that
// never enabled sync — making this an error would force every consumer
// to filter beforehand.
int test_unregister_idempotent_on_unknown_notebook() {
  std::cout << "  Running test_unregister_idempotent_on_unknown_notebook..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  ASSERT_EQ(vxcore_sync_unregister_notebook(ctx, "never-registered-id"), VXCORE_OK);

  vxcore_context_destroy(ctx);
  std::cout << "  \xE2\x9C\x93 test_unregister_idempotent_on_unknown_notebook passed"
            << std::endl;
  return 0;
}

// Core contract: after enable+unregister, the runtime predicate
// is_registered flips back to 0.  vxcore_sync_enable does NOT
// persist sync_* fields to disk on its own — that's the consumer's
// responsibility (Qt SyncService::bootstrapAndPersist calls
// vxcore_notebook_update_config in a separate step).  So this test
// covers runtime-only.  Disk-preservation is exercised separately in
// test_unregister_preserves_disk_state below, which writes JSON
// fields explicitly so it has something concrete to verify.
int test_unregister_clears_runtime_state() {
  std::cout << "  Running test_unregister_clears_runtime_state..." << std::endl;
  const std::string dir = get_test_path("test_unregister_clears_runtime");
  cleanup_test_dir(dir);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, dir.c_str(),
                                   "{\"name\":\"Unregister Cycle\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &id),
            VXCORE_OK);
  ASSERT_NOT_NULL(id);

  ASSERT_EQ(vxcore_sync_enable(
                ctx, id,
                "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}",
                nullptr),
            VXCORE_OK);

  int registered = 0;
  ASSERT_EQ(vxcore_sync_is_registered(ctx, id, &registered), VXCORE_OK);
  ASSERT_EQ(registered, 1);

  ASSERT_EQ(vxcore_sync_unregister_notebook(ctx, id), VXCORE_OK);

  registered = 42;  // poison
  ASSERT_EQ(vxcore_sync_is_registered(ctx, id, &registered), VXCORE_OK);
  ASSERT_EQ(registered, 0);

  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(dir);
  std::cout << "  \xE2\x9C\x93 test_unregister_clears_runtime_state passed" << std::endl;
  return 0;
}

// Pre-write the on-disk sync_* fields via vxcore_notebook_update_config
// (mirrors what Qt's bootstrapAndPersist does after vxcore_sync_enable
// succeeds), then enable+unregister, then read the config back and
// assert the three fields are still set with their original values.
// is_ready, which is a derived predicate over the same fields, must
// also still report 1 after unregister.  If this regresses the user
// sees their persisted sync configuration silently disappear on
// notebook close, even though we only intended to drop the runtime
// libgit2 handle.
int test_unregister_preserves_disk_state() {
  std::cout << "  Running test_unregister_preserves_disk_state..." << std::endl;
  const std::string dir = get_test_path("test_unregister_preserves_disk");
  cleanup_test_dir(dir);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, dir.c_str(),
                                   "{\"name\":\"Preserve Disk\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &id),
            VXCORE_OK);
  ASSERT_NOT_NULL(id);

  // Step 1: persist sync fields to disk (mirrors bootstrapAndPersist).
  const std::string disk_cfg =
      std::string("{\"syncEnabled\":true,") +
      "\"syncBackend\":\"mock\"," +
      "\"syncRemoteUrl\":\"file:///tmp/preserve\"}";
  ASSERT_EQ(vxcore_notebook_update_config(ctx, id, disk_cfg.c_str()), VXCORE_OK);

  int ready_before = 0;
  ASSERT_EQ(vxcore_sync_is_ready(ctx, id, &ready_before), VXCORE_OK);
  ASSERT_EQ(ready_before, 1);

  // Step 2: enable runtime.
  ASSERT_EQ(vxcore_sync_enable(
                ctx, id,
                "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/preserve\"}",
                nullptr),
            VXCORE_OK);

  // Step 3: unregister.  Runtime should clear; disk should remain.
  ASSERT_EQ(vxcore_sync_unregister_notebook(ctx, id), VXCORE_OK);

  int registered_after = 42;
  ASSERT_EQ(vxcore_sync_is_registered(ctx, id, &registered_after), VXCORE_OK);
  ASSERT_EQ(registered_after, 0);

  int ready_after = 0;
  ASSERT_EQ(vxcore_sync_is_ready(ctx, id, &ready_after), VXCORE_OK);
  ASSERT_EQ(ready_after, 1);

  // Step 4: read config JSON and verify all three sync fields survived.
  char *config_json = nullptr;
  ASSERT_EQ(vxcore_notebook_get_config(ctx, id, &config_json), VXCORE_OK);
  ASSERT_NOT_NULL(config_json);

  const nlohmann::json cfg = nlohmann::json::parse(config_json);
  ASSERT_TRUE(cfg.contains("syncEnabled"));
  ASSERT_TRUE(cfg["syncEnabled"].get<bool>());
  ASSERT_TRUE(cfg.contains("syncBackend"));
  ASSERT_EQ(cfg["syncBackend"].get<std::string>(), std::string("mock"));
  ASSERT_TRUE(cfg.contains("syncRemoteUrl"));
  ASSERT_EQ(cfg["syncRemoteUrl"].get<std::string>(),
            std::string("file:///tmp/preserve"));

  vxcore_string_free(config_json);
  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(dir);
  std::cout << "  \xE2\x9C\x93 test_unregister_preserves_disk_state passed" << std::endl;
  return 0;
}

// After unregister, calling enable again with the same params must
// re-register the backend.  This is the path the consumer (VNote) uses
// when the user re-opens the notebook and supplies credentials via the
// Sync Info dialog.
int test_unregister_then_reregister() {
  std::cout << "  Running test_unregister_then_reregister..." << std::endl;
  const std::string dir = get_test_path("test_unregister_then_reregister");
  cleanup_test_dir(dir);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, dir.c_str(),
                                   "{\"name\":\"Reregister\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &id),
            VXCORE_OK);
  ASSERT_NOT_NULL(id);

  ASSERT_EQ(vxcore_sync_enable(
                ctx, id,
                "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/re\"}",
                nullptr),
            VXCORE_OK);
  ASSERT_EQ(vxcore_sync_unregister_notebook(ctx, id), VXCORE_OK);

  int registered = 42;
  ASSERT_EQ(vxcore_sync_is_registered(ctx, id, &registered), VXCORE_OK);
  ASSERT_EQ(registered, 0);

  // Re-enable with the same params: must succeed and put us back in
  // registered=1.
  ASSERT_EQ(vxcore_sync_enable(
                ctx, id,
                "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/re\"}",
                nullptr),
            VXCORE_OK);

  registered = 0;
  ASSERT_EQ(vxcore_sync_is_registered(ctx, id, &registered), VXCORE_OK);
  ASSERT_EQ(registered, 1);

  // And unregister again is still idempotent the second time around.
  ASSERT_EQ(vxcore_sync_unregister_notebook(ctx, id), VXCORE_OK);
  ASSERT_EQ(vxcore_sync_unregister_notebook(ctx, id), VXCORE_OK);

  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(dir);
  std::cout << "  \xE2\x9C\x93 test_unregister_then_reregister passed" << std::endl;
  return 0;
}

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running test_sync_unregister..." << std::endl;
  RUN_TEST(test_unregister_null_pointers);
  RUN_TEST(test_unregister_idempotent_on_unknown_notebook);
  RUN_TEST(test_unregister_clears_runtime_state);
  RUN_TEST(test_unregister_preserves_disk_state);
  RUN_TEST(test_unregister_then_reregister);
  std::cout << "All test_sync_unregister tests passed!" << std::endl;
  return 0;
}
