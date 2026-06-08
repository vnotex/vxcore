// Wave 14: regression tests for vxcore_sync_is_registered.
//
// Background: SyncService::isSyncRegistered (Qt side) previously routed
// through vxcore_sync_get_status -> SyncManager::GetSyncStatus ->
// GitSyncBackend::GetStatus, which acquired the per-backend op_mutex_
// blockingly. That caused a race against worker-thread StageAndCommit /
// FetchRebasePush, manifesting as persistent VXCORE_ERR_SYNC_IN_PROGRESS
// (21) on every Sync Now click.
//
// The fix introduces vxcore_sync_is_registered, a lightweight predicate
// that only acquires SyncManager::state_mutex_ (NEVER the backend
// op_mutex_) and answers "is the notebook in states_?". These tests pin
// down its correctness contract so the property does not silently
// regress.

#include <iostream>
#include <string>

#include "test_internals/mock_sync_backend.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"

// Force-link the mock-backend registration TU (mirrors test_sync.cpp). Without
// this reference MSVC's linker drops mock_sync_backend.cpp and the "mock"
// backend never registers in SyncBackendRegistry, breaking
// vxcore_sync_enable(...,"mock",...).
namespace {
[[maybe_unused]] vxcore::MockSyncBackend *g_force_link_test_internals_mock =
    new vxcore::MockSyncBackend();
}  // namespace

// Null-pointer guard: every required out-param must be non-null. The current
// implementation also rejects null context.
int test_is_registered_null_pointers() {
  std::cout << "  Running test_is_registered_null_pointers..." << std::endl;

  int registered = -1;
  ASSERT_EQ(vxcore_sync_is_registered(nullptr, "id", &registered),
            VXCORE_ERR_NULL_POINTER);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  ASSERT_EQ(vxcore_sync_is_registered(ctx, nullptr, &registered),
            VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_sync_is_registered(ctx, "id", nullptr),
            VXCORE_ERR_NULL_POINTER);

  vxcore_context_destroy(ctx);
  std::cout << "  \xE2\x9C\x93 test_is_registered_null_pointers passed" << std::endl;
  return 0;
}

// Unknown notebook: must report VXCORE_OK + registered=0 (NOT an error
// code). The Qt-side classify() calls this for every loaded notebook on
// every repaint; returning errors would force callers to handle them.
int test_is_registered_unknown_notebook() {
  std::cout << "  Running test_is_registered_unknown_notebook..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  int registered = 42;  // poison value
  ASSERT_EQ(vxcore_sync_is_registered(ctx, "nonexistent-id-xxx", &registered),
            VXCORE_OK);
  ASSERT_EQ(registered, 0);

  vxcore_context_destroy(ctx);
  std::cout << "  \xE2\x9C\x93 test_is_registered_unknown_notebook passed"
            << std::endl;
  return 0;
}

// Notebook exists but sync was NEVER enabled: registered=0.
int test_is_registered_notebook_without_sync() {
  std::cout << "  Running test_is_registered_notebook_without_sync..."
            << std::endl;
  const std::string dir = get_test_path("test_is_registered_no_sync");
  cleanup_test_dir(dir);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, dir.c_str(), "{\"name\":\"No Sync\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &id),
            VXCORE_OK);
  ASSERT_NOT_NULL(id);

  int registered = 42;
  ASSERT_EQ(vxcore_sync_is_registered(ctx, id, &registered), VXCORE_OK);
  ASSERT_EQ(registered, 0);

  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(dir);
  std::cout << "  \xE2\x9C\x93 test_is_registered_notebook_without_sync passed"
            << std::endl;
  return 0;
}

// Sync enabled with mock backend: registered=1. Verifies the OK path that
// SyncStateClassifier::classify() relies on every paint.
int test_is_registered_after_enable() {
  std::cout << "  Running test_is_registered_after_enable..." << std::endl;
  const std::string dir = get_test_path("test_is_registered_enabled");
  cleanup_test_dir(dir);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, dir.c_str(), "{\"name\":\"Enabled\"}",
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

  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(dir);
  std::cout << "  \xE2\x9C\x93 test_is_registered_after_enable passed"
            << std::endl;
  return 0;
}

// Enable then disable: registered must flip back to 0. Mirrors the
// disable-then-re-enable cycle a user can drive from the Sync Info dialog.
int test_is_registered_after_disable() {
  std::cout << "  Running test_is_registered_after_disable..." << std::endl;
  const std::string dir = get_test_path("test_is_registered_disable");
  cleanup_test_dir(dir);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, dir.c_str(),
                                   "{\"name\":\"Disable Cycle\"}",
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

  ASSERT_EQ(vxcore_sync_disable(ctx, id), VXCORE_OK);

  registered = 42;  // poison again
  ASSERT_EQ(vxcore_sync_is_registered(ctx, id, &registered), VXCORE_OK);
  ASSERT_EQ(registered, 0);

  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(dir);
  std::cout << "  \xE2\x9C\x93 test_is_registered_after_disable passed"
            << std::endl;
  return 0;
}

// Independence: enabling sync on notebook A must not change is_registered's
// answer for notebook B. Pins down per-notebook state isolation.
int test_is_registered_per_notebook_isolation() {
  std::cout << "  Running test_is_registered_per_notebook_isolation..."
            << std::endl;
  const std::string dirA = get_test_path("test_is_registered_iso_A");
  const std::string dirB = get_test_path("test_is_registered_iso_B");
  cleanup_test_dir(dirA);
  cleanup_test_dir(dirB);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *idA = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, dirA.c_str(), "{\"name\":\"Iso A\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &idA),
            VXCORE_OK);
  ASSERT_NOT_NULL(idA);
  char *idB = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, dirB.c_str(), "{\"name\":\"Iso B\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &idB),
            VXCORE_OK);
  ASSERT_NOT_NULL(idB);

  ASSERT_EQ(vxcore_sync_enable(
                ctx, idA,
                "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/a\"}",
                nullptr),
            VXCORE_OK);

  int regA = 0;
  int regB = 42;
  ASSERT_EQ(vxcore_sync_is_registered(ctx, idA, &regA), VXCORE_OK);
  ASSERT_EQ(vxcore_sync_is_registered(ctx, idB, &regB), VXCORE_OK);
  ASSERT_EQ(regA, 1);
  ASSERT_EQ(regB, 0);

  vxcore_string_free(idA);
  vxcore_string_free(idB);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(dirA);
  cleanup_test_dir(dirB);
  std::cout << "  \xE2\x9C\x93 test_is_registered_per_notebook_isolation passed"
            << std::endl;
  return 0;
}

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running test_vxcore_sync_is_registered..." << std::endl;
  RUN_TEST(test_is_registered_null_pointers);
  RUN_TEST(test_is_registered_unknown_notebook);
  RUN_TEST(test_is_registered_notebook_without_sync);
  RUN_TEST(test_is_registered_after_enable);
  RUN_TEST(test_is_registered_after_disable);
  RUN_TEST(test_is_registered_per_notebook_isolation);
  std::cout << "All test_vxcore_sync_is_registered tests passed!" << std::endl;
  return 0;
}
