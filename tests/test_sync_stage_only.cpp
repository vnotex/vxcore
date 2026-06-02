// vxcore-sync-stage-only V2: SyncManager::StageOnly / NetworkPhaseOnly tests.
//
// Verifies the stage-only / network-phase split at the SyncManager dispatch
// layer using a FakeSyncBackend that subclasses MockSyncBackend and overrides
// StageAndCommit / FetchRebasePush so we can:
//   (1) prove StageOnly invokes StageAndCommit and does NOT touch the network,
//   (2) prove NetworkPhaseOnly invokes FetchRebasePush (and only that).
//
// Deliberately does NOT touch libgit2 or any real remote.

#include <atomic>
#include <iostream>
#include <memory>
#include <string>

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

using vxcore::ISyncBackend;
using vxcore::MockSyncBackend;
using vxcore::SyncBackendFactory;
using vxcore::SyncConfig;

// Fake backend exposing the new phase split. Inherits the rest of the
// ISyncBackend surface from MockSyncBackend.
class FakeStageBackend : public MockSyncBackend {
 public:
  std::atomic<int> stage_calls{0};
  std::atomic<int> network_calls{0};
  // Whether the network-touching phase ran. Distinct flag from network_calls
  // so the negative-assertion intent is crystal clear at the call site.
  std::atomic<bool> network_was_touched{false};

  VxCoreError next_stage_result = VXCORE_OK;
  VxCoreError next_network_result = VXCORE_OK;
  bool stage_did_commit = true;

  VxCoreError StageAndCommit(bool *out_did_commit) override {
    stage_calls.fetch_add(1);
    if (out_did_commit) *out_did_commit = stage_did_commit;
    return next_stage_result;
  }

  VxCoreError FetchRebasePush() override {
    network_calls.fetch_add(1);
    network_was_touched.store(true);
    return next_network_result;
  }
};

struct NotebookFixture {
  VxCoreContextHandle ctx = nullptr;
  char *notebook_id = nullptr;
  std::string root;

  explicit NotebookFixture(const std::string &name) {
    root = get_test_path(name);
    cleanup_test_dir(root);
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    if (err != VXCORE_OK) { std::cerr << "context_create failed: " << err << "\n"; std::exit(1); }
    err = vxcore_notebook_create(ctx, root.c_str(),
                                 "{\"name\":\"Stage Only Test\"}",
                                 VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
    if (err != VXCORE_OK) { std::cerr << "notebook_create failed: " << err << "\n"; std::exit(1); }
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

// Helper: install fake backend via factory-override path and hand the caller
// the live pointer so they can assert call counts after dispatch.
FakeStageBackend *enable_with_fake(NotebookFixture &nb) {
  FakeStageBackend *out_ptr = nullptr;
  SyncBackendFactory factory =
      [&out_ptr](const SyncConfig &,
                 std::shared_ptr<vxcore::ICredentialProvider>)
      -> std::unique_ptr<ISyncBackend> {
    auto mock = std::make_unique<FakeStageBackend>();
    mock->SetName("__fake_stage__");
    out_ptr = mock.get();
    return mock;
  };
  SyncConfig cfg;
  cfg.backend = "git";  // bypassed by factory override
  cfg.remote_url = "test://stage";
  VxCoreError err = nb.sync_manager().EnableSyncWithFactoryForTesting(
      nb.notebook_id, cfg, nullptr, factory);
  if (err != VXCORE_OK) { std::cerr << "EnableSync failed: " << err << "\n"; std::exit(1); }
  return out_ptr;
}

// Subtest 1: StageOnly invokes StageAndCommit but does NOT touch the network.
int stage_only_does_not_touch_network() {
  std::cout << "  Running stage_only_does_not_touch_network..." << std::endl;
  NotebookFixture nb("test_stage_only_no_network");
  FakeStageBackend *fake = enable_with_fake(nb);
  ASSERT_NOT_NULL(fake);

  bool did_commit = false;
  VxCoreError err = nb.sync_manager().StageOnly(nb.notebook_id, nullptr, &did_commit);
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT_EQ(fake->stage_calls.load(), 1);
  ASSERT_TRUE(did_commit);
  // The load-bearing negative assertion: no network whatsoever.
  ASSERT_EQ(fake->network_calls.load(), 0);
  ASSERT_FALSE(fake->network_was_touched.load());

  VxCoreError dis = vxcore_sync_disable(nb.ctx, nb.notebook_id);
  ASSERT_EQ(dis, VXCORE_OK);
  std::cout << "  PASS" << std::endl;
  return 0;
}

// Subtest 2: NetworkPhaseOnly invokes FetchRebasePush and only that.
int network_phase_only_calls_fetch_rebase_push() {
  std::cout << "  Running network_phase_only_calls_fetch_rebase_push..." << std::endl;
  NotebookFixture nb("test_stage_only_network_only");
  FakeStageBackend *fake = enable_with_fake(nb);
  ASSERT_NOT_NULL(fake);

  VxCoreError err = nb.sync_manager().NetworkPhaseOnly(nb.notebook_id, nullptr);
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT_EQ(fake->network_calls.load(), 1);
  ASSERT_TRUE(fake->network_was_touched.load());
  // And the stage phase was NOT implicitly invoked.
  ASSERT_EQ(fake->stage_calls.load(), 0);

  VxCoreError dis = vxcore_sync_disable(nb.ctx, nb.notebook_id);
  ASSERT_EQ(dis, VXCORE_OK);
  std::cout << "  PASS" << std::endl;
  return 0;
}

// Subtest 3: backend errors propagate through StageOnly.
int stage_only_propagates_backend_error() {
  std::cout << "  Running stage_only_propagates_backend_error..." << std::endl;
  NotebookFixture nb("test_stage_only_err");
  FakeStageBackend *fake = enable_with_fake(nb);
  ASSERT_NOT_NULL(fake);
  fake->next_stage_result = VXCORE_ERR_UNKNOWN;

  bool did_commit = true;
  VxCoreError err = nb.sync_manager().StageOnly(nb.notebook_id, nullptr, &did_commit);
  ASSERT_EQ(err, VXCORE_ERR_UNKNOWN);
  ASSERT_EQ(fake->stage_calls.load(), 1);
  ASSERT_EQ(fake->network_calls.load(), 0);
  ASSERT_FALSE(fake->network_was_touched.load());

  VxCoreError dis = vxcore_sync_disable(nb.ctx, nb.notebook_id);
  ASSERT_EQ(dis, VXCORE_OK);
  std::cout << "  PASS" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  std::cout << "Running SyncManager stage-only / network-phase tests..." << std::endl;
  RUN_TEST(stage_only_does_not_touch_network);
  RUN_TEST(network_phase_only_calls_fetch_rebase_push);
  RUN_TEST(stage_only_propagates_backend_error);
  std::cout << "All stage-only tests passed." << std::endl;
  return 0;
}
