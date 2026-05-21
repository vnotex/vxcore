// Task 7.5 (F3.2): coverage for SyncManager::GetSyncConfig cache semantics.
// The cache (configs_cache_) is a read-through mirror of the per-notebook
// NotebookConfig sync_* JSON fields. Two invariants under test:
//   1. EnableSync persists the SyncConfig into the cache; subsequent
//      GetSyncConfig calls return the cached SyncConfig unchanged even if
//      the underlying NotebookConfig JSON is mutated out-of-band.
//   2. InvalidateConfigCache forces the next GetSyncConfig to re-read from
//      the authoritative NotebookConfig JSON, surfacing out-of-band edits.

#include "test_utils.h"
#include "vxcore/vxcore.h"

#include <string>

#include "core/context.h"
#include "core/notebook.h"
#include "core/notebook_manager.h"
#include "sync/sync_manager.h"
#include "sync/sync_types.h"
#include "test_internals/mock_sync_backend.h"

// Force-link the test_internals TU so its anonymous-namespace
// BackendRegistration token runs at static-init time and the "mock" backend
// lands in SyncBackendRegistry::Instance(). Mirrors the pattern used in
// test_sync_status_queries.cpp.
namespace {
[[maybe_unused]] vxcore::MockSyncBackend *g_force_link_test_internals_mock =
    new vxcore::MockSyncBackend();
}  // namespace

namespace {

// Persist the notebook's sync_* config fields directly via the C API. This is
// what the Qt SyncService layer does in production; tests need the same path
// so SyncManager::GetSyncConfig's cache-miss branch reads non-default values.
VxCoreError PersistSyncFields(VxCoreContextHandle ctx, const char *notebook_id,
                              const std::string &backend, const std::string &remote_url,
                              int interval_seconds) {
  std::string json = "{\"syncEnabled\":true,\"syncBackend\":\"" + backend +
                     "\",\"syncRemoteUrl\":\"" + remote_url +
                     "\",\"syncIntervalSeconds\":" + std::to_string(interval_seconds) + "}";
  return vxcore_notebook_update_config(ctx, notebook_id, json.c_str());
}

}  // namespace

int test_write_persists_then_caches() {
  std::cout << "  Running test_write_persists_then_caches..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("cache_persists_then_caches_nb");
  cleanup_test_dir(nb_path);
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(),
                                    "{\"name\":\"CachePersistsThenCaches\"}",
                                    VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  // EnableSync (via C API) populates the cache with backend="mock",
  // interval=60 (default in SyncConfig).
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"mock://orig\","
                               "\"intervalSeconds\":42}",
                               nullptr),
            VXCORE_OK);

  // Direct C++ access to verify cache behavior.
  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);

  // First GetSyncConfig: cache hit, returns the EnableSync-supplied config.
  vxcore::SyncConfig cfg;
  ASSERT_EQ(vctx->sync_manager->GetSyncConfig(notebook_id, cfg), VXCORE_OK);
  ASSERT(cfg.backend == "mock");
  ASSERT(cfg.remote_url == "mock://orig");
  ASSERT_EQ(cfg.interval_seconds, 42);

  // Mutate the underlying NotebookConfig JSON out-of-band. Without
  // invalidation, the cache must continue to win — proving that "write
  // persists to cache" and that reads do not silently fall through to disk.
  ASSERT_EQ(PersistSyncFields(ctx, notebook_id, "mock", "mock://changed", 999), VXCORE_OK);

  vxcore::SyncConfig cfg2;
  ASSERT_EQ(vctx->sync_manager->GetSyncConfig(notebook_id, cfg2), VXCORE_OK);
  ASSERT(cfg2.backend == "mock");
  ASSERT(cfg2.remote_url == "mock://orig");  // cached, NOT disk value
  ASSERT_EQ(cfg2.interval_seconds, 42);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  PASS test_write_persists_then_caches" << std::endl;
  return 0;
}

int test_invalidate_forces_reread() {
  std::cout << "  Running test_invalidate_forces_reread..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  std::string nb_path = get_test_path("cache_invalidate_reread_nb");
  cleanup_test_dir(nb_path);
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(),
                                    "{\"name\":\"CacheInvalidateReread\"}",
                                    VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);

  // Pre-populate the persisted NotebookConfig fields (no EnableSync — this
  // simulates an S4 notebook where the disk has the truth but the runtime
  // cache is empty).
  ASSERT_EQ(PersistSyncFields(ctx, notebook_id, "mock", "mock://v1", 17), VXCORE_OK);

  // First GetSyncConfig: cache miss, populates cache from disk.
  vxcore::SyncConfig cfg1;
  ASSERT_EQ(vctx->sync_manager->GetSyncConfig(notebook_id, cfg1), VXCORE_OK);
  ASSERT(cfg1.backend == "mock");
  ASSERT(cfg1.remote_url == "mock://v1");
  ASSERT_EQ(cfg1.interval_seconds, 17);

  // Mutate the underlying NotebookConfig JSON out-of-band.
  ASSERT_EQ(PersistSyncFields(ctx, notebook_id, "mock", "mock://v2", 23), VXCORE_OK);

  // Without invalidation: cache wins, returns v1.
  vxcore::SyncConfig cfg2;
  ASSERT_EQ(vctx->sync_manager->GetSyncConfig(notebook_id, cfg2), VXCORE_OK);
  ASSERT(cfg2.remote_url == "mock://v1");
  ASSERT_EQ(cfg2.interval_seconds, 17);

  // Invalidate then re-read: cache miss forces fresh disk read; v2 surfaces.
  vctx->sync_manager->InvalidateConfigCache(notebook_id);
  vxcore::SyncConfig cfg3;
  ASSERT_EQ(vctx->sync_manager->GetSyncConfig(notebook_id, cfg3), VXCORE_OK);
  ASSERT(cfg3.backend == "mock");
  ASSERT(cfg3.remote_url == "mock://v2");
  ASSERT_EQ(cfg3.interval_seconds, 23);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  PASS test_invalidate_forces_reread" << std::endl;
  return 0;
}

int main() {
  RUN_TEST(test_write_persists_then_caches);
  RUN_TEST(test_invalidate_forces_reread);
  std::cout << "\nAll test_sync_config_cache tests passed!" << std::endl;
  return 0;
}
