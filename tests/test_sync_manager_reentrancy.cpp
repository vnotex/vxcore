// Task 10.1 (sync-backend-phase4 F2.4 part 2): SyncManager reentrancy test.
//
// Verifies that SyncManager::state_mutex_ is RELEASED before backend method
// invocations, so a backend (or hook) may call back into SyncManager from
// inside Sync()/Initialize() without deadlocking.
//
// Why this matters: Wave 0.5 contract forbids holding any sync mutex across
// external invocation (hook, signal, libgit2 callback, credential provider,
// backend method). If state_mutex_ were held across backend->Sync(), the
// recursive call into SyncManager::GetSyncConfig — which re-acquires
// state_mutex_ — would deadlock indefinitely on std::mutex (non-recursive
// by contract).
//
// Test strategy:
//   1. Inject a custom backend via EnableSyncWithFactoryForTesting.
//   2. Backend's Sync() calls back into SyncManager::GetSyncConfig.
//   3. Run TriggerSync on a worker thread with a 5-second watchdog timer.
//   4. Assert TriggerSync returned within budget (no deadlock).
//
// Hook semantics are NOT exercised directly here — vxcore does not have
// hooks; the Qt-side HookManager lives outside the sync subsystem. The
// backend-callback path is the equivalent re-entry surface for vxcore
// itself and exercises the exact same state_mutex_ release contract.

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "core/context.h"
#include "sync/credential_provider.h"
#include "sync/sync_backend.h"
#include "sync/sync_manager.h"
#include "sync/sync_types.h"
#include "test_internals/mock_sync_backend.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_types.h"

namespace {

// Force-link the test_internals TU so its anonymous-namespace
// BackendRegistration token runs at static-init time. Matches the pattern
// used in test_event_manager.cpp and test_sync_manager_factory_override.cpp.
[[maybe_unused]] vxcore::MockSyncBackend *g_force_link_test_internals_mock =
    new vxcore::MockSyncBackend();

using vxcore::ISyncBackend;
using vxcore::SyncBackendFactory;
using vxcore::SyncConfig;
using vxcore::SyncManager;

// ReentrantBackend: a minimal ISyncBackend whose Sync() (and Initialize()
// when configured) calls back into SyncManager::GetSyncConfig. If
// state_mutex_ were held across the backend invocation, the recursive
// GetSyncConfig call would deadlock on std::mutex.
class ReentrantBackend : public ISyncBackend {
 public:
  ReentrantBackend(SyncManager *mgr, std::string notebook_id, bool reenter_on_initialize)
      : mgr_(mgr),
        notebook_id_(std::move(notebook_id)),
        reenter_on_initialize_(reenter_on_initialize) {}

  std::string GetName() const override { return "reentrant"; }
  vxcore::SyncCapabilities GetCapabilities() const override {
    return static_cast<vxcore::SyncCapabilities>(vxcore::SyncCapability::None);
  }
  bool IsInitialized() const override { return initialized_; }

  VxCoreError Initialize(const std::string &, const SyncConfig &) override {
    if (reenter_on_initialize_) {
      // Re-enter SyncManager from inside Initialize. This is the critical
      // path: EnableSyncImpl previously seeded configs_cache_ + states_
      // under state_mutex_ and would have deadlocked here if the lock
      // were still held across this call.
      SyncConfig out;
      auto err = mgr_->GetSyncConfig(notebook_id_, out);
      reentry_initialize_err_ = err;
      reentry_initialize_called_ = true;
    }
    initialized_ = true;
    return VXCORE_OK;
  }

  VxCoreError Sync(vxcore::SyncProgressCallback, void *) override {
    // Re-enter SyncManager from inside Sync. TriggerSync sets
    // states_[id] = kStaging under state_mutex_ before invoking this
    // method; if it held the lock across this call, the recursive
    // GetSyncConfig (and the implicit InvalidateConfigCache contract)
    // would deadlock on std::mutex.
    SyncConfig out;
    auto err = mgr_->GetSyncConfig(notebook_id_, out);
    reentry_sync_err_ = err;
    reentry_sync_called_ = true;
    return VXCORE_OK;
  }

  VxCoreError GetStatus(std::vector<vxcore::SyncFileInfo> &) override {
    return VXCORE_OK;
  }
  VxCoreError GetConflicts(std::vector<vxcore::SyncConflictInfo> &) override {
    return VXCORE_OK;
  }
  VxCoreError ResolveConflict(const std::string &,
                                       vxcore::SyncConflictResolution) override {
    return VXCORE_OK;
  }

  bool reentry_initialize_called() const { return reentry_initialize_called_; }
  bool reentry_sync_called() const { return reentry_sync_called_; }
  VxCoreError reentry_initialize_err() const { return reentry_initialize_err_; }
  VxCoreError reentry_sync_err() const { return reentry_sync_err_; }

 private:
  SyncManager *mgr_;
  std::string notebook_id_;
  bool reenter_on_initialize_;
  bool initialized_ = false;
  std::atomic<bool> reentry_initialize_called_{false};
  std::atomic<bool> reentry_sync_called_{false};
  std::atomic<VxCoreError> reentry_initialize_err_{VXCORE_OK};
  std::atomic<VxCoreError> reentry_sync_err_{VXCORE_OK};
};

// Fixture: spin up a context + bundled notebook + access to its SyncManager.
struct NotebookFixture {
  VxCoreContextHandle ctx = nullptr;
  char *notebook_id = nullptr;
  std::string root;

  explicit NotebookFixture(const std::string &name) {
    root = get_test_path(name);
    cleanup_test_dir(root);
    auto err = vxcore_context_create(nullptr, &ctx);
    if (err != VXCORE_OK) {
      std::cerr << "context_create failed: " << err << std::endl;
      std::exit(1);
    }
    err = vxcore_notebook_create(ctx, root.c_str(),
                                 "{\"name\":\"Reentrancy Test\"}",
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

  SyncManager &sync_manager() {
    auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
    return *vctx->sync_manager;
  }
};

// Run `fn` on a worker thread, return true iff it completes within `budget`.
template <typename F>
bool run_with_watchdog(F fn, std::chrono::seconds budget) {
  std::promise<void> done;
  auto fut = done.get_future();
  std::thread worker([&]() {
    fn();
    done.set_value();
  });
  bool finished = (fut.wait_for(budget) == std::future_status::ready);
  if (finished) {
    worker.join();
  } else {
    // Deadlocked. Detach so we exit; the process will be torn down at
    // test-runner termination. We deliberately do NOT join() — that would
    // hang forever and mask the failure as a CTest timeout.
    worker.detach();
  }
  return finished;
}

// Subtest 1: backend re-enters SyncManager::GetSyncConfig from Sync().
// TriggerSync must complete within 5 seconds, proving state_mutex_ was
// released before backend->Sync() was invoked.
int test_hook_can_call_back_into_sync_manager() {
  std::cout << "  Running test_hook_can_call_back_into_sync_manager..." << std::endl;
  NotebookFixture nb("test_sync_reentrancy_sync");

  ReentrantBackend *backend_raw = nullptr;
  SyncBackendFactory factory =
      [&](const SyncConfig &,
          std::shared_ptr<vxcore::ICredentialProvider>) -> std::unique_ptr<ISyncBackend> {
    auto backend = std::make_unique<ReentrantBackend>(
        &nb.sync_manager(), std::string(nb.notebook_id), /*reenter_on_initialize=*/false);
    backend_raw = backend.get();
    return backend;
  };

  SyncConfig cfg;
  cfg.backend = "git";  // Name doesn't matter — factory overrides.
  cfg.remote_url = "test://reentrancy";

  auto err = nb.sync_manager().EnableSyncWithFactoryForTesting(
      nb.notebook_id, cfg, nullptr, factory);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(backend_raw);

  VxCoreError trigger_err = VXCORE_OK;
  bool finished = run_with_watchdog(
      [&]() { trigger_err = nb.sync_manager().TriggerSync(nb.notebook_id); },
      std::chrono::seconds(5));

  ASSERT_TRUE(finished);  // No deadlock.
  ASSERT_EQ(trigger_err, VXCORE_OK);
  ASSERT_TRUE(backend_raw->reentry_sync_called());
  ASSERT_EQ(backend_raw->reentry_sync_err(), VXCORE_OK);

  std::cout << "  ✓ test_hook_can_call_back_into_sync_manager passed" << std::endl;
  return 0;
}

// Subtest 2: backend re-enters SyncManager::GetSyncConfig from Initialize().
// EnableSync must complete within 5 seconds, proving state_mutex_ was
// released before backend->Initialize() was invoked.
int test_backend_can_call_back_from_initialize() {
  std::cout << "  Running test_backend_can_call_back_from_initialize..." << std::endl;
  NotebookFixture nb("test_sync_reentrancy_initialize");

  ReentrantBackend *backend_raw = nullptr;
  SyncBackendFactory factory =
      [&](const SyncConfig &,
          std::shared_ptr<vxcore::ICredentialProvider>) -> std::unique_ptr<ISyncBackend> {
    auto backend = std::make_unique<ReentrantBackend>(
        &nb.sync_manager(), std::string(nb.notebook_id), /*reenter_on_initialize=*/true);
    backend_raw = backend.get();
    return backend;
  };

  SyncConfig cfg;
  cfg.backend = "git";
  cfg.remote_url = "test://reentrancy-init";

  VxCoreError enable_err = VXCORE_OK;
  bool finished = run_with_watchdog(
      [&]() {
        enable_err = nb.sync_manager().EnableSyncWithFactoryForTesting(
            nb.notebook_id, cfg, nullptr, factory);
      },
      std::chrono::seconds(5));

  ASSERT_TRUE(finished);  // No deadlock.
  ASSERT_EQ(enable_err, VXCORE_OK);
  ASSERT_NOT_NULL(backend_raw);
  ASSERT_TRUE(backend_raw->reentry_initialize_called());
  ASSERT_EQ(backend_raw->reentry_initialize_err(), VXCORE_OK);

  std::cout << "  ✓ test_backend_can_call_back_from_initialize passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  std::cout << "Running SyncManager reentrancy tests..." << std::endl;
  RUN_TEST(test_hook_can_call_back_into_sync_manager);
  RUN_TEST(test_backend_can_call_back_from_initialize);
  std::cout << "All SyncManager reentrancy tests passed." << std::endl;
  return 0;
}
