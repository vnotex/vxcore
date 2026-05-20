// Task 5.1 (sync-backend-phase4 F4.2): SyncManager factory-override unit tests.
//
// Verifies that SyncManager::EnableSyncImpl, when called via the
// EnableSyncWithFactoryForTesting hook, uses the supplied factory to
// construct the backend instead of routing through SyncBackendRegistry.
//
// These tests deliberately do NOT touch libgit2 or the network — the
// injected MockSyncBackend provides a deterministic in-memory backend with
// configurable identity (name/capabilities/initialized) so we can prove
// the override actually wired through a different concrete instance than
// the registry would have produced for the same backend name.

#include <atomic>
#include <iostream>
#include <memory>
#include <string>

#include "core/context.h"
#include "sync/credential_provider.h"
#include "sync/sync_backend.h"
#include "sync/sync_backend_registry.h"
#include "sync/sync_manager.h"
#include "sync/sync_types.h"
#include "test_internals/mock_sync_backend.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_types.h"

namespace {

using vxcore::ISyncBackend;
using vxcore::MockSyncBackend;
using vxcore::SyncBackendFactory;
using vxcore::SyncConfig;

// Helper: spin up a fresh context with one bundled notebook ready for sync.
struct NotebookFixture {
  VxCoreContextHandle ctx = nullptr;
  char *notebook_id = nullptr;
  std::string root;

  explicit NotebookFixture(const std::string &name) {
    root = get_test_path(name);
    cleanup_test_dir(root);
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    if (err != VXCORE_OK) {
      std::cerr << "context_create failed: " << err << std::endl;
      std::exit(1);
    }
    err = vxcore_notebook_create(ctx, root.c_str(),
                                 "{\"name\":\"Factory Override Test\"}",
                                 VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
    if (err != VXCORE_OK) {
      std::cerr << "notebook_create failed: " << err << std::endl;
      std::exit(1);
    }
  }

  ~NotebookFixture() {
    if (notebook_id) vxcore_string_free(notebook_id);
    if (ctx) vxcore_context_destroy(ctx);
    cleanup_test_dir(root);
  }

  vxcore::SyncManager &sync_manager() {
    auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
    return *vctx->sync_manager;
  }
};

// Subtest 1: when factory_override is non-null, it constructs the backend
// (not the registry). We prove this by routing a backend name ("git") that
// the registry CAN resolve to a real GitSyncBackend through our mock factory,
// then triggering GetSyncStatus and observing the mock's RecordCall trail.
int mock_backend_used_when_factory_provided() {
  std::cout << "  Running mock_backend_used_when_factory_provided..." << std::endl;
  NotebookFixture nb("test_factory_override_mock");

  std::atomic<int> factory_call_count{0};
  MockSyncBackend *constructed_mock_ptr = nullptr;

  SyncBackendFactory factory =
      [&factory_call_count, &constructed_mock_ptr](
          const SyncConfig &,
          std::shared_ptr<vxcore::ICredentialProvider>) -> std::unique_ptr<ISyncBackend> {
    factory_call_count.fetch_add(1);
    auto mock = std::make_unique<MockSyncBackend>();
    mock->SetName("__test_factory_override_sentinel");
    constructed_mock_ptr = mock.get();
    return mock;
  };

  SyncConfig cfg;
  // Use "git" backend name on purpose: if the override were ignored and the
  // registry path ran, it would construct a real GitSyncBackend (or pass the
  // libgit2 ok() guard). Instead the factory must intercept and produce a mock.
  cfg.backend = "git";
  cfg.remote_url = "test://override";

  VxCoreError err = nb.sync_manager().EnableSyncWithFactoryForTesting(
      nb.notebook_id, cfg, nullptr, factory);
  ASSERT_EQ(err, VXCORE_OK);

  // Sentinel 1: factory was invoked exactly once during EnableSync.
  ASSERT_EQ(factory_call_count.load(), 1);
  ASSERT_NOT_NULL(constructed_mock_ptr);

  // Sentinel 2: the registered backend IS our mock (driving it through a
  // SyncManager call records the invocation on the mock instance the factory
  // returned — proves the mock survived as the dispatch target).
  vxcore::SyncState out_state = vxcore::SyncState::kIdle;
  std::vector<vxcore::SyncFileInfo> out_files;
  err = nb.sync_manager().GetSyncStatus(nb.notebook_id, out_state, out_files);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_TRUE(constructed_mock_ptr->GetCallCount() >= 1);
  bool saw_get_status = false;
  for (const auto &rec : constructed_mock_ptr->GetCallRecords()) {
    if (rec.find("GetStatus") != std::string::npos) {
      saw_get_status = true;
      break;
    }
  }
  ASSERT_TRUE(saw_get_status);

  err = vxcore_sync_disable(nb.ctx, nb.notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::cout << "  PASS" << std::endl;
  return 0;
}

// Subtest 2: when factory_override is non-null, the unknown-backend allow-list
// is bypassed. Registry path would reject "__nonexistent_backend__" with
// VXCORE_ERR_UNKNOWN_BACKEND; factory path must succeed.
int factory_bypasses_unknown_backend_guard() {
  std::cout << "  Running factory_bypasses_unknown_backend_guard..." << std::endl;
  NotebookFixture nb("test_factory_override_bypass");

  SyncBackendFactory factory =
      [](const SyncConfig &,
         std::shared_ptr<vxcore::ICredentialProvider>) -> std::unique_ptr<ISyncBackend> {
    return std::make_unique<MockSyncBackend>();
  };

  SyncConfig cfg;
  cfg.backend = "__nonexistent_backend__";  // would fail allow-list on registry path
  cfg.remote_url = "test://bypass";

  VxCoreError err = nb.sync_manager().EnableSyncWithFactoryForTesting(
      nb.notebook_id, cfg, nullptr, factory);
  ASSERT_EQ(err, VXCORE_OK);

  // Sanity: registry path with the same name should fail with UNKNOWN_BACKEND.
  // First disable so we can re-enable.
  err = vxcore_sync_disable(nb.ctx, nb.notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_sync_enable(nb.ctx, nb.notebook_id, "{\"backend\":\"__nonexistent_backend__\",\"remoteUrl\":\"test://x\"}", nullptr);
  ASSERT_EQ(err, VXCORE_ERR_UNKNOWN_BACKEND);

  std::cout << "  PASS" << std::endl;
  return 0;
}

// Subtest 3: when factory_override returns nullptr, EnableSync returns
// VXCORE_ERR_UNKNOWN_BACKEND and rolls back configs_/states_.
int factory_returning_null_rolls_back() {
  std::cout << "  Running factory_returning_null_rolls_back..." << std::endl;
  NotebookFixture nb("test_factory_override_null");

  SyncBackendFactory null_factory =
      [](const SyncConfig &,
         std::shared_ptr<vxcore::ICredentialProvider>) -> std::unique_ptr<ISyncBackend> {
    return nullptr;
  };

  SyncConfig cfg;
  cfg.backend = "git";
  cfg.remote_url = "test://null";

  VxCoreError err = nb.sync_manager().EnableSyncWithFactoryForTesting(
      nb.notebook_id, cfg, nullptr, null_factory);
  ASSERT_EQ(err, VXCORE_ERR_UNKNOWN_BACKEND);

  // Rollback verification: GetSyncStatus must report SYNC_NOT_ENABLED because
  // EnableSync removed the partial configs_/states_ entries on failure.
  vxcore::SyncState out_state = vxcore::SyncState::kIdle;
  std::vector<vxcore::SyncFileInfo> out_files;
  err = nb.sync_manager().GetSyncStatus(nb.notebook_id, out_state, out_files);
  ASSERT_EQ(err, VXCORE_ERR_SYNC_NOT_ENABLED);

  std::cout << "  PASS" << std::endl;
  return 0;
}

}  // namespace

// Local no-op shim — tests linked via add_vxcore_test() get the real symbol
// from vxcore.dll, so the include above resolves it. Nothing to declare here.

int main() {
  vxcore_set_test_mode(1);
  std::cout << "Running SyncManager factory-override tests..." << std::endl;
  RUN_TEST(mock_backend_used_when_factory_provided);
  RUN_TEST(factory_bypasses_unknown_backend_guard);
  RUN_TEST(factory_returning_null_rolls_back);
  std::cout << "All SyncManager factory-override tests passed." << std::endl;
  return 0;
}
