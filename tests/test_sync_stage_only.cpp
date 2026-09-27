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
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "core/context.h"
#include "core/event_manager.h"
#include "core/event_names.h"
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
  std::function<void()> on_stage;
  std::function<void()> on_sync;
  std::function<void()> on_capabilities;
  std::function<VxCoreError(const std::vector<std::string> &, std::vector<std::string> &)> on_apply;
  vxcore::SyncCancellationPtr cancellation;

  vxcore::SyncCapabilities GetCapabilities() const override {
    if (on_capabilities) on_capabilities();
    return MockSyncBackend::GetCapabilities();
  }

  void SetCancellation(vxcore::SyncCancellationPtr token) override {
    cancellation = std::move(token);
  }

  VxCoreError Sync(vxcore::SyncProgressCallback, void *) override {
    if (on_sync) on_sync();
    return VXCORE_OK;
  }

  VxCoreError ApplySync(const std::vector<std::string> &protected_paths,
                        std::vector<std::string> &changed_paths) override {
    if (on_apply) return on_apply(protected_paths, changed_paths);
    return ISyncBackend::ApplySync(protected_paths, changed_paths);
  }

  VxCoreError StageAndCommit(bool *out_did_commit) override {
    stage_calls.fetch_add(1);
    if (on_stage) on_stage();
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

  explicit NotebookFixture(const std::string &name,
                           VxCoreNotebookType type = VXCORE_NOTEBOOK_BUNDLED) {
    root = get_test_path(name);
    cleanup_test_dir(root);
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    if (err != VXCORE_OK) { std::cerr << "context_create failed: " << err << "\n"; std::exit(1); }
    err = vxcore_notebook_create(ctx, root.c_str(), "{\"name\":\"Stage Only Test\"}", type,
                                 &notebook_id);
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

int capabilities_and_default_apply() {
  NotebookFixture nb("test_sync_capabilities_apply");
  auto *fake = enable_with_fake(nb);
  const uint32_t capabilities = static_cast<uint32_t>(vxcore::SyncCapability::DeferredLocalApply) |
                                static_cast<uint32_t>(vxcore::SyncCapability::ConflictDetection);
  fake->SetCapabilities(capabilities);
  bool reentered = false;
  fake->on_capabilities = [&] {
    SyncConfig config;
    reentered = nb.sync_manager().GetSyncConfig(nb.notebook_id, config) == VXCORE_OK;
  };
  uint32_t actual = 0;
  ASSERT_EQ(vxcore_sync_get_capabilities(nb.ctx, nb.notebook_id, &actual), VXCORE_OK);
  ASSERT_EQ(actual, capabilities);
  ASSERT_TRUE(reentered);

  // A backend without DeferredLocalApply has no local transaction to install.
  fake->SetCapabilities(0);
  std::vector<std::string> changed{"stale"};
  ASSERT_EQ(nb.sync_manager().ApplyPhaseOnly(nb.notebook_id, nullptr, {}, changed), VXCORE_OK);
  ASSERT_TRUE(changed.empty());
  char *output = nullptr;
  ASSERT_EQ(vxcore_sync_apply_phase(nb.ctx, nb.notebook_id, nullptr, nullptr, &output), VXCORE_OK);
  ASSERT_NOT_NULL(output);
  ASSERT(nlohmann::json::parse(output) == nlohmann::json::array());
  vxcore_string_free(output);
  ASSERT_EQ(fake->stage_calls.load(), 0);
  ASSERT_EQ(fake->network_calls.load(), 0);
  return 0;
}

int apply_validates_paths_before_mutation() {
  NotebookFixture nb("test_sync_apply_paths");
  auto *fake = enable_with_fake(nb);
  bool applied = false;
  fake->on_apply = [&](const auto &, auto &) {
    applied = true;
    write_file(nb.root + "/unexpected.md", "must not be written");
    return VXCORE_OK;
  };
  std::vector<const char *> invalid = {
      "",
      "[",
      "{}",
      "null",
      "\"note.md\"",
      "[1]",
      "[null]",
      "[true]",
      "[{}]",
      "[[\"note.md\"]]",
      "[\"\"]",
      "[\".\"]",
      "[\"..\"]",
      "[\"../note.md\"]",
      "[\"folder/../note.md\"]",
      "[\"folder/./note.md\"]",
      "[\"/note.md\"]",
      "[\"C:/note.md\"]",
      "[\"C:note.md\"]",
      "[\"//server/share/note.md\"]",
      "[\"folder\\\\note.md\"]",
      "[\"note\\u0000.md\"]",
      "[\"folder//note.md\"]",
      "[\"folder/\"]",
      "[\"note.md\",false]",
      "[\"\\ud800\"]",
      "[\"\xff\"]",
  };
#ifdef _WIN32
  invalid.insert(invalid.end(), {"[\"folder/CON.md\"]", "[\"aux\"]", "[\"COM1.txt\"]",
                                 "[\"lpt\\u00b9\"]", "[\"note.md \"]", "[\"folder./note.md\"]",
                                 "[\"bad?.md\"]", "[\"bad\\u0001.md\"]"});
#endif
  for (const auto *paths : invalid) {
    char sentinel[] = "stale";
    char *output = sentinel;
    ASSERT_EQ(vxcore_sync_apply_phase(nb.ctx, nb.notebook_id, nullptr, paths, &output),
              VXCORE_ERR_INVALID_PARAM);
    ASSERT_NULL(output);
  }
  ASSERT_FALSE(applied);
  ASSERT_FALSE(path_exists(nb.root + "/unexpected.md"));
  return 0;
}

int apply_protects_paths_and_reports_partial_failure() {
  NotebookFixture nb("test_sync_apply_partial");
  auto *fake = enable_with_fake(nb);
  const std::string protected_path = u8"草稿 #%.md";
  write_file(nb.root + "/" + protected_path, "unsaved draft");
  fake->on_apply = [&](const auto &protected_paths, auto &changed) -> VxCoreError {
    SyncConfig config;
    if (nb.sync_manager().GetSyncConfig(nb.notebook_id, config) != VXCORE_OK) {
      return VXCORE_ERR_INVALID_STATE;
    }
    // This concrete side effect makes both Unicode path preservation and
    // partial-failure reporting visible at the C ABI boundary.
    for (const auto &path : {protected_path, std::string("installed.md")}) {
      if (std::find(protected_paths.begin(), protected_paths.end(), path) !=
          protected_paths.end()) {
        continue;
      }
      write_file(nb.root + "/" + path, "incoming");
      changed.push_back(path);
    }
    return VXCORE_ERR_IO;
  };
  const auto paths = nlohmann::json::array({protected_path}).dump();
  char *output = nullptr;
  ASSERT_EQ(vxcore_sync_apply_phase(nb.ctx, nb.notebook_id, nullptr, paths.c_str(), &output),
            VXCORE_ERR_IO);
  ASSERT_NOT_NULL(output);
  ASSERT(nlohmann::json::parse(output) == nlohmann::json::array({"installed.md"}));
  vxcore_string_free(output);
  std::ifstream protected_file(utf8_to_fs_path(nb.root + "/" + protected_path));
  std::string body;
  std::getline(protected_file, body);
  ASSERT_EQ(body, std::string("unsaved draft"));
  std::ifstream installed(utf8_to_fs_path(nb.root + "/installed.md"));
  std::getline(installed, body);
  ASSERT_EQ(body, std::string("incoming"));
  return 0;
}

int apply_cancellation_lifetime_and_exception_cleanup() {
  NotebookFixture nb("test_sync_apply_cancel");
  auto *fake = enable_with_fake(nb);
  auto *token = vxcore_sync_create_cancellation();
  ASSERT_NOT_NULL(token);
  vxcore_sync_cancel(token);
  bool applied = false;
  fake->on_apply = [&](const auto &, auto &) {
    applied = true;
    return VXCORE_OK;
  };
  char *output = nullptr;
  ASSERT_EQ(vxcore_sync_apply_phase(nb.ctx, nb.notebook_id, token, "[]", &output),
            VXCORE_ERR_CANCELLED);
  ASSERT_FALSE(applied);
  ASSERT_NOT_NULL(output);
  ASSERT(nlohmann::json::parse(output).empty());
  vxcore_string_free(output);
  vxcore_sync_free_cancellation(token);

  token = vxcore_sync_create_cancellation();
  ASSERT_NOT_NULL(token);
  std::weak_ptr<vxcore::SyncCancellation> weak;
  bool alive_after_free = false;
  fake->on_apply = [&](const auto &, auto &changed) -> VxCoreError {
    weak = fake->cancellation;
    vxcore_sync_free_cancellation(token);
    token = nullptr;
    alive_after_free = !weak.expired();
    write_file(nb.root + "/partial.md", "installed before exception");
    changed.push_back("partial.md");
    throw std::runtime_error("apply failure");
  };
  output = nullptr;
  ASSERT_EQ(vxcore_sync_apply_phase(nb.ctx, nb.notebook_id, token, "[]", &output),
            VXCORE_ERR_UNKNOWN);
  ASSERT_TRUE(alive_after_free);
  ASSERT_TRUE(weak.expired());
  ASSERT_FALSE(fake->cancellation);
  ASSERT_NOT_NULL(output);
  ASSERT(nlohmann::json::parse(output) == nlohmann::json::array({"partial.md"}));
  vxcore_string_free(output);
  ASSERT_TRUE(path_exists(nb.root + "/partial.md"));
  return 0;
}

int phase_queries_initialize_outputs_on_errors() {
  NotebookFixture nb("test_sync_apply_errors");
  uint32_t capabilities = 123;
  ASSERT_EQ(vxcore_sync_get_capabilities(nullptr, nb.notebook_id, &capabilities),
            VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(capabilities, 0u);
  ASSERT_EQ(vxcore_sync_get_capabilities(nb.ctx, nullptr, &capabilities), VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_sync_get_capabilities(nb.ctx, nb.notebook_id, nullptr), VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_sync_get_capabilities(nb.ctx, "unknown", &capabilities), VXCORE_ERR_NOT_FOUND);
  ASSERT_EQ(vxcore_sync_get_capabilities(nb.ctx, nb.notebook_id, &capabilities),
            VXCORE_ERR_SYNC_NOT_ENABLED);
  char sentinel[] = "stale";
  char *output = sentinel;
  ASSERT_EQ(vxcore_sync_apply_phase(nullptr, nb.notebook_id, nullptr, nullptr, &output),
            VXCORE_ERR_NULL_POINTER);
  ASSERT_NULL(output);
  output = sentinel;
  ASSERT_EQ(vxcore_sync_apply_phase(nb.ctx, nullptr, nullptr, nullptr, &output),
            VXCORE_ERR_NULL_POINTER);
  ASSERT_NULL(output);
  ASSERT_EQ(vxcore_sync_apply_phase(nb.ctx, nb.notebook_id, nullptr, nullptr, nullptr),
            VXCORE_ERR_NULL_POINTER);
  for (const auto &entry :
       {std::make_pair("unknown", VXCORE_ERR_NOT_FOUND),
        std::make_pair(static_cast<const char *>(nb.notebook_id), VXCORE_ERR_SYNC_NOT_ENABLED)}) {
    output = nullptr;
    ASSERT_EQ(vxcore_sync_apply_phase(nb.ctx, entry.first, nullptr, nullptr, &output),
              entry.second);
    ASSERT_NOT_NULL(output);
    ASSERT(nlohmann::json::parse(output) == nlohmann::json::array());
    vxcore_string_free(output);
  }

  enable_with_fake(nb);
  auto &backends = nb.sync_manager().BackendsForTesting();
  auto backend = std::move(backends.at(nb.notebook_id));
  backends.erase(nb.notebook_id);
  capabilities = 123;
  ASSERT_EQ(vxcore_sync_get_capabilities(nb.ctx, nb.notebook_id, &capabilities),
            VXCORE_ERR_NOT_IMPLEMENTED);
  ASSERT_EQ(capabilities, 0u);
  std::vector<std::string> changed{"stale"};
  ASSERT_EQ(nb.sync_manager().ApplyPhaseOnly(nb.notebook_id, nullptr, {}, changed),
            VXCORE_ERR_NOT_IMPLEMENTED);
  ASSERT_TRUE(changed.empty());
  backends.emplace(nb.notebook_id, std::move(backend));
  return 0;
}

int deferred_snapshot_detects_mutation_and_preserves_new_work() {
  NotebookFixture nb("test_sync_deferred_revision");
  auto *fake = enable_with_fake(nb);
  fake->SetCapabilities(static_cast<uint32_t>(vxcore::SyncCapability::DeferredLocalApply));
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(nb.ctx);
  const auto mark_dirty = [&] {
    ctx->event_manager->Emit(vxcore::events::kFileSaved, {{"notebookId", nb.notebook_id}});
  };
  fake->on_stage = mark_dirty;
  bool committed = false;
  ASSERT_EQ(nb.sync_manager().StageOnly(nb.notebook_id, nullptr, &committed),
            VXCORE_ERR_SYNC_IN_PROGRESS);
  ASSERT_EQ(fake->network_calls.load(), 0);
  ASSERT(nb.sync_manager().GetDirtyNotebooks() == std::vector<std::string>{nb.notebook_id});
  fake->on_stage = nullptr;
  ASSERT_EQ(nb.sync_manager().StageOnly(nb.notebook_id, nullptr, &committed), VXCORE_OK);

  fake->on_apply = [&](const auto &, auto &) {
    mark_dirty();
    return VXCORE_OK;
  };
  ASSERT_EQ(nb.sync_manager().TriggerSync(nb.notebook_id), VXCORE_OK);
  ASSERT(nb.sync_manager().GetDirtyNotebooks() == std::vector<std::string>{nb.notebook_id});
  fake->on_apply = nullptr;
  ASSERT_EQ(nb.sync_manager().TriggerSync(nb.notebook_id), VXCORE_OK);
  ASSERT_TRUE(nb.sync_manager().GetDirtyNotebooks().empty());

  // Existing non-deferred scheduling retains its original success-clears policy.
  fake->SetCapabilities(0);
  fake->on_sync = mark_dirty;
  ASSERT_EQ(nb.sync_manager().TriggerSync(nb.notebook_id), VXCORE_OK);
  ASSERT_TRUE(nb.sync_manager().GetDirtyNotebooks().empty());
  return 0;
}

int apply_reservation_blocks_writes_only_in_its_notebook() {
  NotebookFixture nb("test_sync_apply_reservation");
  char *other = nullptr, *file = nullptr, *buffer = nullptr;
  const auto other_root = nb.root + "_other";
  cleanup_test_dir(other_root);
  ASSERT_EQ(vxcore_notebook_create(nb.ctx, other_root.c_str(), "{\"name\":\"Other\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &other),
            VXCORE_OK);
  ASSERT_EQ(vxcore_file_create(nb.ctx, nb.notebook_id, "", "note.md", &file), VXCORE_OK);
  vxcore_string_free(file);
  ASSERT_EQ(vxcore_buffer_open(nb.ctx, nb.notebook_id, "note.md", &buffer), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_set_content_raw(nb.ctx, buffer, "draft", 5), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_write_backup(nb.ctx, buffer), VXCORE_OK);
  ASSERT_EQ(vxcore_sync_set_apply_in_progress(nullptr, nb.notebook_id, 1), VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_sync_set_apply_in_progress(nb.ctx, "unknown", 1), VXCORE_ERR_NOT_FOUND);
  ASSERT_EQ(vxcore_sync_set_apply_in_progress(nb.ctx, nb.notebook_id, 2), VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_sync_set_apply_in_progress(nb.ctx, nb.notebook_id, 1), VXCORE_OK);
  ASSERT_EQ(vxcore_sync_set_apply_in_progress(nb.ctx, nb.notebook_id, 1), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(nb.ctx, buffer), VXCORE_ERR_SYNC_IN_PROGRESS);
  ASSERT_EQ(vxcore_buffer_write_backup(nb.ctx, buffer), VXCORE_ERR_SYNC_IN_PROGRESS);
  ASSERT_EQ(vxcore_buffer_recover_backup(nb.ctx, buffer), VXCORE_ERR_SYNC_IN_PROGRESS);
  ASSERT_EQ(vxcore_buffer_discard_backup(nb.ctx, buffer), VXCORE_ERR_SYNC_IN_PROGRESS);
  char *output = nullptr;
  ASSERT_EQ(vxcore_buffer_insert_asset_raw(nb.ctx, buffer, "image.png", "x", 1, &output),
            VXCORE_ERR_SYNC_IN_PROGRESS);
  ASSERT_NULL(output);
  ASSERT_EQ(vxcore_file_create(nb.ctx, nb.notebook_id, "", "blocked.md", &output),
            VXCORE_ERR_SYNC_IN_PROGRESS);
  ASSERT_EQ(vxcore_folder_create(nb.ctx, nb.notebook_id, "", "blocked", &output),
            VXCORE_ERR_SYNC_IN_PROGRESS);
  ASSERT_EQ(vxcore_notebook_close(nb.ctx, nb.notebook_id), VXCORE_ERR_SYNC_IN_PROGRESS);
  ASSERT_EQ(vxcore_file_create(nb.ctx, other, "", "allowed.md", &output), VXCORE_OK);
  vxcore_string_free(output);
  ASSERT_EQ(vxcore_notebook_set_read_only(nb.ctx, nb.notebook_id, true), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(nb.ctx, buffer), VXCORE_ERR_READ_ONLY);
  ASSERT_EQ(vxcore_sync_set_apply_in_progress(nb.ctx, nb.notebook_id, 0), VXCORE_OK);
  ASSERT_EQ(vxcore_sync_set_apply_in_progress(nb.ctx, nb.notebook_id, 0), VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_set_read_only(nb.ctx, nb.notebook_id, false), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(nb.ctx, buffer), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_close(nb.ctx, buffer), VXCORE_OK);
  vxcore_string_free(buffer);
  ASSERT_EQ(vxcore_notebook_close(nb.ctx, other), VXCORE_OK);
  vxcore_string_free(other);
  cleanup_test_dir(other_root);
  return 0;
}

int deferred_refresh_reloads_metadata_without_local_mutation() {
  NotebookFixture nb("test_sync_apply_refresh");
  auto *fake = enable_with_fake(nb);
  fake->SetCapabilities(static_cast<uint32_t>(vxcore::SyncCapability::DeferredLocalApply));
  char *file = nullptr, *output = nullptr;
  ASSERT_EQ(vxcore_file_create(nb.ctx, nb.notebook_id, "", "before.md", &file), VXCORE_OK);
  vxcore_string_free(file);
  ASSERT_EQ(vxcore_folder_list_children(nb.ctx, nb.notebook_id, "", &output), VXCORE_OK);
  vxcore_string_free(output);
  const auto config_path = nb.root + "/vx_notebook/config.json";
  const auto folder_path = nb.root + "/vx_notebook/contents/vx.json";
  std::ifstream config_file(utf8_to_fs_path(config_path));
  auto config = nlohmann::json::parse(config_file);
  config_file.close();
  config["name"] = "Incoming name";
  config["unknownRemoteField"] = 73;
  std::ifstream folder_file(utf8_to_fs_path(folder_path));
  auto folder = nlohmann::json::parse(folder_file);
  folder_file.close();
  folder["files"][0]["name"] = "incoming.md";
  fake->on_apply = [&](const auto &, auto &changed) {
    write_file(config_path, config.dump());
    write_file(folder_path, folder.dump());
    write_file(nb.root + "/incoming.md", "incoming");
    changed = {"vx_notebook/config.json", "vx_notebook/contents/vx.json", "incoming.md"};
    return VXCORE_OK;
  };
  nb.sync_manager().SetLastSyncTime(nb.notebook_id, 123456);
  ASSERT_EQ(nb.sync_manager().StageOnly(nb.notebook_id, nullptr, nullptr), VXCORE_OK);
  ASSERT_EQ(nb.sync_manager().NetworkPhaseOnly(nb.notebook_id, nullptr), VXCORE_OK);
  ASSERT_EQ(vxcore_sync_set_apply_in_progress(nb.ctx, nb.notebook_id, 1), VXCORE_OK);
  output = nullptr;
  ASSERT_EQ(vxcore_sync_apply_phase(nb.ctx, nb.notebook_id, nullptr, "[]", &output), VXCORE_OK);
  vxcore_string_free(output);
  ASSERT_FALSE(nb.sync_manager().GetDirtyNotebooks().empty());
  vxcore::SyncState state;
  std::vector<vxcore::SyncFileInfo> files;
  ASSERT_EQ(nb.sync_manager().GetSyncStatus(nb.notebook_id, state, files), VXCORE_OK);
  ASSERT(state != vxcore::SyncState::kIdle);
  ASSERT_EQ(vxcore_sync_refresh_notebook(nb.ctx, nb.notebook_id), VXCORE_OK);
  ASSERT_TRUE(nb.sync_manager().GetDirtyNotebooks().empty());
  ASSERT_EQ(nb.sync_manager().LastSyncTime(nb.notebook_id), 123456);
  ASSERT_EQ(vxcore_notebook_get_config(nb.ctx, nb.notebook_id, &output), VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(output)["name"].get<std::string>(), "Incoming name");
  vxcore_string_free(output);
  ASSERT_EQ(vxcore_folder_list_children(nb.ctx, nb.notebook_id, "", &output), VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(output)["files"][0]["name"].get<std::string>(), "incoming.md");
  vxcore_string_free(output);
  std::ifstream unchanged_config(utf8_to_fs_path(config_path));
  ASSERT(nlohmann::json::parse(unchanged_config) == config);
  ASSERT_EQ(vxcore_sync_set_apply_in_progress(nb.ctx, nb.notebook_id, 0), VXCORE_OK);
  return 0;
}

int deferred_refresh_preserves_failure_cancellation_and_new_edits() {
  NotebookFixture nb("test_sync_apply_finalize");
  auto *fake = enable_with_fake(nb);
  fake->SetCapabilities(static_cast<uint32_t>(vxcore::SyncCapability::DeferredLocalApply));
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(nb.ctx);
  const auto mark = [&] {
    ctx->event_manager->Emit(vxcore::events::kFileSaved, {{"notebookId", nb.notebook_id}});
  };
  const auto apply = [&](VxCoreSyncCancellation *token) {
    char *output = nullptr;
    const auto error = vxcore_sync_apply_phase(nb.ctx, nb.notebook_id, token, "[]", &output);
    vxcore_string_free(output);
    return error;
  };
  mark();
  ASSERT_EQ(nb.sync_manager().StageOnly(nb.notebook_id, nullptr, nullptr), VXCORE_OK);
  ASSERT_EQ(vxcore_sync_refresh_notebook(nb.ctx, nb.notebook_id), VXCORE_ERR_SYNC_IN_PROGRESS);
  ASSERT_FALSE(nb.sync_manager().GetDirtyNotebooks().empty());
  fake->on_apply = [](const auto &, auto &) { return VXCORE_ERR_SYNC_CONFLICT; };
  ASSERT_EQ(apply(nullptr), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(vxcore_sync_refresh_notebook(nb.ctx, nb.notebook_id), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_FALSE(nb.sync_manager().GetDirtyNotebooks().empty());
  fake->on_apply = nullptr;
  ASSERT_EQ(nb.sync_manager().StageOnly(nb.notebook_id, nullptr, nullptr), VXCORE_OK);
  auto *token = vxcore_sync_create_cancellation();
  ASSERT_EQ(apply(token), VXCORE_OK);
  vxcore_sync_cancel(token);
  vxcore_sync_free_cancellation(token);
  ASSERT_EQ(vxcore_sync_refresh_notebook(nb.ctx, nb.notebook_id), VXCORE_ERR_CANCELLED);
  ASSERT_FALSE(nb.sync_manager().GetDirtyNotebooks().empty());
  ASSERT_EQ(nb.sync_manager().StageOnly(nb.notebook_id, nullptr, nullptr), VXCORE_OK);
  ASSERT_EQ(apply(nullptr), VXCORE_OK);
  mark();
  ASSERT_EQ(vxcore_sync_refresh_notebook(nb.ctx, nb.notebook_id), VXCORE_OK);
  ASSERT_FALSE(nb.sync_manager().GetDirtyNotebooks().empty());
  ASSERT_EQ(nb.sync_manager().StageOnly(nb.notebook_id, nullptr, nullptr), VXCORE_OK);
  ASSERT_EQ(apply(nullptr), VXCORE_OK);
  // With no metadata changes finalization does not parse/rebuild notebook metadata.
  write_file(nb.root + "/vx_notebook/config.json", "invalid JSON");
  ASSERT_EQ(vxcore_sync_refresh_notebook(nb.ctx, nb.notebook_id), VXCORE_OK);
  ASSERT_TRUE(nb.sync_manager().GetDirtyNotebooks().empty());
  return 0;
}

int deferred_partial_refresh_keeps_original_error_and_timestamp() {
  NotebookFixture nb("test_sync_partial_refresh");
  auto *fake = enable_with_fake(nb);
  fake->SetCapabilities(static_cast<uint32_t>(vxcore::SyncCapability::DeferredLocalApply));
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(nb.ctx);
  ctx->event_manager->Emit(vxcore::events::kFileSaved, {{"notebookId", nb.notebook_id}});
  nb.sync_manager().SetLastSyncTime(nb.notebook_id, 987654);
  const auto path = nb.root + "/vx_notebook/config.json";
  std::ifstream input(utf8_to_fs_path(path));
  auto config = nlohmann::json::parse(input);
  input.close();
  fake->on_apply = [&](const auto &, auto &changed) {
    config["name"] = "Partially installed";
    write_file(path, config.dump());
    changed = {"vx_notebook/config.json"};
    return VXCORE_ERR_IO;
  };
  ASSERT_EQ(nb.sync_manager().StageOnly(nb.notebook_id, nullptr, nullptr), VXCORE_OK);
  std::vector<std::string> changed;
  ASSERT_EQ(nb.sync_manager().ApplyPhaseOnly(nb.notebook_id, nullptr, {}, changed), VXCORE_ERR_IO);
  ASSERT_EQ(vxcore_sync_refresh_notebook(nb.ctx, nb.notebook_id), VXCORE_ERR_IO);
  char *output = nullptr;
  ASSERT_EQ(vxcore_notebook_get_config(nb.ctx, nb.notebook_id, &output), VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(output)["name"].get<std::string>(), "Partially installed");
  vxcore_string_free(output);
  ASSERT_EQ(nb.sync_manager().LastSyncTime(nb.notebook_id), 987654);
  ASSERT_FALSE(nb.sync_manager().GetDirtyNotebooks().empty());
  fake->on_apply = [&](const auto &, auto &paths) {
    write_file(path, "{\"id\":\"not-this-notebook\",\"name\":\"Rejected\"}");
    paths = {"vx_notebook/config.json"};
    return VXCORE_OK;
  };
  ASSERT_EQ(nb.sync_manager().StageOnly(nb.notebook_id, nullptr, nullptr), VXCORE_OK);
  ASSERT_EQ(nb.sync_manager().ApplyPhaseOnly(nb.notebook_id, nullptr, {}, changed), VXCORE_OK);
  ASSERT_EQ(vxcore_sync_refresh_notebook(nb.ctx, nb.notebook_id), VXCORE_ERR_INVALID_STATE);
  ASSERT_EQ(nb.sync_manager().LastSyncTime(nb.notebook_id), 987654);
  ASSERT_FALSE(nb.sync_manager().GetDirtyNotebooks().empty());
  return 0;
}

int conflict_keep_both_is_serialized() {
  NotebookFixture nb("test_sync_conflict_capability");
  auto *fake = enable_with_fake(nb);
  vxcore::SyncConflictInfo ordinary;
  ordinary.path = "image.png";
  ordinary.is_binary = true;
  vxcore::SyncConflictInfo protected_conflict;
  protected_conflict.path = "private.md.vne";
  protected_conflict.is_binary = true;
  protected_conflict.can_keep_both = false;
  fake->SetConflicts({ordinary, protected_conflict});
  char *output = nullptr;
  ASSERT_EQ(vxcore_sync_get_conflicts(nb.ctx, nb.notebook_id, &output), VXCORE_OK);
  const auto conflicts = nlohmann::json::parse(output)["conflicts"];
  vxcore_string_free(output);
  ASSERT_TRUE(conflicts[0]["canKeepBoth"].get<bool>());
  ASSERT_FALSE(conflicts[1]["canKeepBoth"].get<bool>());
  return 0;
}

int raw_notebooks_reject_new_phases() {
  NotebookFixture nb("test_sync_apply_raw", VXCORE_NOTEBOOK_RAW);
  uint32_t capabilities = 123;
  ASSERT_EQ(vxcore_sync_get_capabilities(nb.ctx, nb.notebook_id, &capabilities),
            VXCORE_ERR_UNSUPPORTED);
  ASSERT_EQ(capabilities, 0u);
  char *output = nullptr;
  ASSERT_EQ(vxcore_sync_apply_phase(nb.ctx, nb.notebook_id, nullptr, "[]", &output),
            VXCORE_ERR_UNSUPPORTED);
  ASSERT_NOT_NULL(output);
  ASSERT(nlohmann::json::parse(output) == nlohmann::json::array());
  vxcore_string_free(output);
  ASSERT_EQ(vxcore_sync_set_apply_in_progress(nb.ctx, nb.notebook_id, 1), VXCORE_ERR_UNSUPPORTED);
  ASSERT_EQ(vxcore_sync_refresh_notebook(nb.ctx, nb.notebook_id), VXCORE_ERR_UNSUPPORTED);
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  std::cout << "Running SyncManager stage-only / network-phase tests..." << std::endl;
  RUN_TEST(stage_only_does_not_touch_network);
  RUN_TEST(network_phase_only_calls_fetch_rebase_push);
  RUN_TEST(stage_only_propagates_backend_error);
  RUN_TEST(capabilities_and_default_apply);
  RUN_TEST(apply_validates_paths_before_mutation);
  RUN_TEST(apply_protects_paths_and_reports_partial_failure);
  RUN_TEST(apply_cancellation_lifetime_and_exception_cleanup);
  RUN_TEST(phase_queries_initialize_outputs_on_errors);
  RUN_TEST(deferred_snapshot_detects_mutation_and_preserves_new_work);
  RUN_TEST(apply_reservation_blocks_writes_only_in_its_notebook);
  RUN_TEST(deferred_refresh_reloads_metadata_without_local_mutation);
  RUN_TEST(deferred_refresh_preserves_failure_cancellation_and_new_edits);
  RUN_TEST(deferred_partial_refresh_keeps_original_error_and_timestamp);
  RUN_TEST(conflict_keep_both_is_serialized);
  RUN_TEST(raw_notebooks_reject_new_phases);
  std::cout << "All stage-only tests passed." << std::endl;
  return 0;
}
