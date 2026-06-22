// T10 of auto-sync-on-save plan: verify that vxcore_buffer_save fires
// `file.saved` events through BufferManager → EventManager → SyncManager,
// marks the parent notebook dirty AND triggers a `sync.should_run` emit
// (which consumers like VNote translate into a sync job — vxcore itself no
// longer owns dispatch via the named "sync" WorkQueue; that path was
// removed in commit cba4e21 per src/sync/AGENTS.md). Subtests cover the
// happy path plus all "should NOT emit" edge cases: non-sync notebook,
// failed save, virtual buffer, external buffer. Subtest 6 locks in the
// default of the boolean auto-sync gate: SyncConfig::auto_sync_enabled and
// NotebookConfig::auto_sync_enabled both default to true.
//
// Structural template: test_sync_manager_dispatch.cpp (MockSyncBackend +
// RegisterBackendForTesting + reinterpret_cast<VxCoreContext *> pattern).
// Assertion patterns: test_event_manager.cpp:419-607 (dirty-tracking gold
// standard via GetDirtyNotebooks()) + Subscribe("sync.should_run", ...)
// for emission counting.

#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/context.h"
#include "core/event_manager.h"
#include "core/event_names.h"
#include "mock_sync_backend.h"
#include "sync/sync_backend.h"
#include "sync/sync_manager.h"
#include "sync/sync_types.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_types.h"

using namespace vxcore;

namespace {

// Helper: write a small content blob into an open buffer so SaveBuffer has
// something to flush to disk.
VxCoreError WriteBufferContent(VxCoreContextHandle ctx, const char *buffer_id,
                               const std::string &text) {
  return vxcore_buffer_set_content_raw(ctx, buffer_id, text.data(), text.size());
}

// Per-notebook sync.should_run counter shared across subtests. The
// emission contract is: SyncManager::mark_dirty -> MaybeEnqueueSync ->
// Emit(kSyncShouldRun, {"notebookId": <id>}). We filter by notebookId so a
// stray emit for another test notebook in the same context cannot pollute
// the assertion.
struct SyncShouldRunCounter {
  std::atomic<int> count{0};
  std::string filter_notebook_id;
  vxcore::EventManager::ListenerId listener_id = 0;
};

// Install a sync.should_run subscriber that increments counter.count
// only for events whose notebookId matches counter.filter_notebook_id (or
// for ALL emits when the filter is empty).
void InstallSyncShouldRunCounter(VxCoreContextHandle ctx, SyncShouldRunCounter &counter) {
  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  counter.listener_id = vctx->event_manager->Subscribe(
      vxcore::events::kSyncShouldRun,
      [&counter](const std::string &, const nlohmann::json &data) {
        if (counter.filter_notebook_id.empty()) {
          counter.count.fetch_add(1);
          return;
        }
        const auto it = data.find("notebookId");
        if (it != data.end() && it->is_string() &&
            it->get<std::string>() == counter.filter_notebook_id) {
          counter.count.fetch_add(1);
        }
      });
}

void UninstallSyncShouldRunCounter(VxCoreContextHandle ctx, SyncShouldRunCounter &counter) {
  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  if (counter.listener_id != 0) {
    vctx->event_manager->Unsubscribe(counter.listener_id);
    counter.listener_id = 0;
  }
}

}  // namespace

// ----------------------------------------------------------------------------
// Subtest 1: happy path — save on a sync-enabled notebook marks dirty AND
// triggers at least one sync.should_run emit (the event vxcore now uses to
// hand dispatch to consumers per src/sync/AGENTS.md).
// ----------------------------------------------------------------------------
int test_save_emits_for_sync_enabled_notebook() {
  std::cout << "  Running test_save_emits_for_sync_enabled_notebook..." << std::endl;
  vxcore_set_test_mode(1);
  std::string nb_path = get_test_path("test_save_emit_sync_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"SaveEmitNb\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  // Task 5.2 (F4.1): "mock" is now self-registered via BackendRegistration in
  // test_internals, so the registry path works. Use the public C API.
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}",
                               nullptr),
            VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);

  // Pre-condition: no dirty notebooks yet (enable does not mark dirty).
  ASSERT_TRUE(ctx_impl->sync_manager->GetDirtyNotebooks().empty());

  // Install the sync.should_run counter BEFORE any mutation so we capture
  // both file.created and file.saved emissions on the test notebook.
  SyncShouldRunCounter counter;
  counter.filter_notebook_id = notebook_id;
  InstallSyncShouldRunCounter(ctx, counter);

  // Create a file in the notebook root. file.created -> mark_dirty ->
  // sync.should_run (since the notebook is sync-enabled).
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "save_emit.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  // Open a buffer for the file, write content, save. file.saved emit fires
  // here.
  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, notebook_id, "save_emit.md", &buffer_id), VXCORE_OK);
  ASSERT_EQ(WriteBufferContent(ctx, buffer_id, "hello world"), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(ctx, buffer_id), VXCORE_OK);

  // Assert: notebook is in the dirty set.
  auto dirty = ctx_impl->sync_manager->GetDirtyNotebooks();
  ASSERT_EQ(dirty.size(), 1u);
  ASSERT_EQ(dirty[0], std::string(notebook_id));

  // Assert: at least one sync.should_run was emitted for THIS notebook.
  // mark_dirty has no debounce gate (cba4e21), so both file.created and
  // file.saved should fire one emit each — but we only require >=1 to keep
  // the test resilient to emission-coalescing tweaks within vxcore.
  ASSERT_TRUE(counter.count.load() >= 1);

  UninstallSyncShouldRunCounter(ctx, counter);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  \xE2\x9C\x93 test_save_emits_for_sync_enabled_notebook passed" << std::endl;
  return 0;
}

// ----------------------------------------------------------------------------
// Subtest 2: save on a notebook WITHOUT sync enabled does NOT mark dirty and
// does NOT trigger any sync.should_run emit.
// ----------------------------------------------------------------------------
int test_save_no_emit_for_non_sync_notebook() {
  std::cout << "  Running test_save_no_emit_for_non_sync_notebook..." << std::endl;
  vxcore_set_test_mode(1);
  std::string nb_path = get_test_path("test_save_emit_nosync_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"NoSyncNb\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  // Do NOT call vxcore_sync_enable — SyncManager's configs_ map has no entry
  // for this notebook so mark_dirty is a no-op even when the event fires.

  SyncShouldRunCounter counter;
  counter.filter_notebook_id = notebook_id;
  InstallSyncShouldRunCounter(ctx, counter);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "nosync.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, notebook_id, "nosync.md", &buffer_id), VXCORE_OK);
  ASSERT_EQ(WriteBufferContent(ctx, buffer_id, "no sync content"), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(ctx, buffer_id), VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ASSERT_TRUE(ctx_impl->sync_manager->GetDirtyNotebooks().empty());
  ASSERT_EQ(counter.count.load(), 0);

  UninstallSyncShouldRunCounter(ctx, counter);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  \xE2\x9C\x93 test_save_no_emit_for_non_sync_notebook passed" << std::endl;
  return 0;
}

// ----------------------------------------------------------------------------
// Subtest 3: a failed save (VXCORE_ERR_IO) MUST NOT emit file.saved (and
// therefore MUST NOT trigger sync.should_run for the failed save). Force
// the failure by replacing the on-disk file with a directory of the same
// name before calling save — ofstream::open then fails because the path is
// a directory, BufferManager::SaveBuffer returns VXCORE_ERR_IO before
// reaching the EmitEvent line, and SyncManager never sees the event.
// ----------------------------------------------------------------------------
int test_failed_save_does_not_emit() {
  std::cout << "  Running test_failed_save_does_not_emit..." << std::endl;
  vxcore_set_test_mode(1);
  std::string nb_path = get_test_path("test_save_emit_fail_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"SaveFailNb\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}",
                               nullptr),
            VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "willfail.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, notebook_id, "willfail.md", &buffer_id), VXCORE_OK);
  ASSERT_EQ(WriteBufferContent(ctx, buffer_id, "content that will fail to save"), VXCORE_OK);

  // Clear dirty state from the setup-time file.created emission BEFORE
  // installing the counter so the failed save is observed in isolation.
  ctx_impl->sync_manager->ClearDirty(notebook_id);
  ASSERT_TRUE(ctx_impl->sync_manager->GetDirtyNotebooks().empty());

  SyncShouldRunCounter counter;
  counter.filter_notebook_id = notebook_id;
  InstallSyncShouldRunCounter(ctx, counter);

  // Replace the file with a directory at the same path so ofstream::open
  // fails. This works cross-platform.
  std::string file_full_path = nb_path + "/willfail.md";
  std::error_code ec;
  std::filesystem::remove(utf8_to_fs_path(file_full_path), ec);
  std::filesystem::create_directory(utf8_to_fs_path(file_full_path), ec);

  // Save must fail with VXCORE_ERR_IO and must NOT emit.
  ASSERT_EQ(vxcore_buffer_save(ctx, buffer_id), VXCORE_ERR_IO);

  // After failed save, dirty state must remain empty (no event emitted)
  // AND no sync.should_run emit was triggered.
  ASSERT_TRUE(ctx_impl->sync_manager->GetDirtyNotebooks().empty());
  ASSERT_EQ(counter.count.load(), 0);

  UninstallSyncShouldRunCounter(ctx, counter);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  \xE2\x9C\x93 test_failed_save_does_not_emit passed" << std::endl;
  return 0;
}

// ----------------------------------------------------------------------------
// Subtest 4: virtual buffers short-circuit in BufferManager::SaveBuffer
// (the early `if (buffer->IsVirtual()) return VXCORE_OK;`) before reaching
// the EmitEvent line, so file.saved MUST NOT fire even when sync is enabled
// on another notebook in the same context.
// ----------------------------------------------------------------------------
int test_virtual_buffer_save_does_not_emit() {
  std::cout << "  Running test_virtual_buffer_save_does_not_emit..." << std::endl;
  vxcore_set_test_mode(1);
  std::string nb_path = get_test_path("test_save_emit_virtual_nb");
  cleanup_test_dir(nb_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  // Create a sync-enabled notebook so a stray emit WOULD be observable.
  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"VirtualEmitNb\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);
  ASSERT_EQ(vxcore_sync_enable(ctx, notebook_id,
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}",
                               nullptr),
            VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);

  // Install counter with NO filter so even a stray cross-notebook emit
  // would be visible. (Virtual buffers carry no notebook id, so a stray
  // emit could in principle land with an empty notebookId payload.)
  SyncShouldRunCounter counter;
  InstallSyncShouldRunCounter(ctx, counter);

  // Open a virtual buffer (no filesystem backing) and save it.
  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open_virtual(ctx, "vx://settings", &buffer_id), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(ctx, buffer_id), VXCORE_OK);

  // No emit: dirty set is empty, no sync.should_run fired.
  ASSERT_TRUE(ctx_impl->sync_manager->GetDirtyNotebooks().empty());
  ASSERT_EQ(counter.count.load(), 0);

  UninstallSyncShouldRunCounter(ctx, counter);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  \xE2\x9C\x93 test_virtual_buffer_save_does_not_emit passed" << std::endl;
  return 0;
}

// ----------------------------------------------------------------------------
// Subtest 5: external buffers (opened with empty notebook id) skip the emit
// branch in BufferManager::SaveBuffer (`if (!nb_id.empty())`). Verifies that
// saving a standalone file does not mark any notebook dirty and does not
// trigger sync.should_run.
// ----------------------------------------------------------------------------
int test_external_buffer_save_does_not_emit() {
  std::cout << "  Running test_external_buffer_save_does_not_emit..." << std::endl;
  vxcore_set_test_mode(1);
  std::string ext_dir = get_test_path("test_save_emit_external");
  cleanup_test_dir(ext_dir);
  std::error_code ec;
  std::filesystem::create_directories(utf8_to_fs_path(ext_dir), ec);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  // Subscribe with no filter — any sync.should_run anywhere in this
  // context counts.
  SyncShouldRunCounter counter;
  InstallSyncShouldRunCounter(ctx, counter);

  // Open a buffer for a standalone file outside any notebook.
  // Pass nullptr for notebook_id (the API treats it as "" — external file).
  std::string standalone_file = ext_dir + "/standalone.md";
  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, nullptr, standalone_file.c_str(), &buffer_id), VXCORE_OK);
  ASSERT_EQ(WriteBufferContent(ctx, buffer_id, "external content"), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(ctx, buffer_id), VXCORE_OK);

  // No notebook id → no emit → no dirty notebook → no sync.should_run.
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ASSERT_TRUE(ctx_impl->sync_manager->GetDirtyNotebooks().empty());
  ASSERT_EQ(counter.count.load(), 0);

  UninstallSyncShouldRunCounter(ctx, counter);
  vxcore_string_free(buffer_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(ext_dir);
  std::cout << "  \xE2\x9C\x93 test_external_buffer_save_does_not_emit passed" << std::endl;
  return 0;
}

// ----------------------------------------------------------------------------
// Subtest 6: lock in the boolean auto-sync gate defaults (T4: SyncConfig,
// T5: NotebookConfig). SyncConfig is tested directly because its FromJson +
// ctor are header-inlined. NotebookConfig is tested via the C API
// (vxcore_notebook_create + vxcore_notebook_get_config) because its ctor +
// FromJson are non-exported out-of-line symbols inside vxcore.dll.
// ----------------------------------------------------------------------------
int test_defaults_auto_sync_enabled() {
  std::cout << "  Running test_defaults_auto_sync_enabled..." << std::endl;

  // Default-constructed SyncConfig — auto_sync_enabled defaults to true.
  SyncConfig cfg_default;
  ASSERT_TRUE(cfg_default.auto_sync_enabled);

  // FromJson with no autoSyncEnabled key preserves the default.
  nlohmann::json sync_json = nlohmann::json::parse("{\"backend\":\"git\"}");
  SyncConfig cfg_from_json = SyncConfig::FromJson(sync_json);
  ASSERT_TRUE(cfg_from_json.auto_sync_enabled);

  // NotebookConfig default — verified via the C API to avoid linking against
  // non-exported symbols.
  std::string nb_path = get_test_path("test_defaults_nb");
  cleanup_test_dir(nb_path);
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  // Create notebook with only "name" set — no autoSyncEnabled key.
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"DefaultsTest\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *config_json = nullptr;
  ASSERT_EQ(vxcore_notebook_get_config(ctx, notebook_id, &config_json), VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT_TRUE(config.contains("autoSyncEnabled"));
  ASSERT_TRUE(config["autoSyncEnabled"].is_boolean());
  ASSERT_EQ(config["autoSyncEnabled"].get<bool>(), true);

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  \xE2\x9C\x93 test_defaults_auto_sync_enabled passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running buffer-save dirty-tracking tests...\n";
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();
  RUN_TEST(test_save_emits_for_sync_enabled_notebook);
  RUN_TEST(test_save_no_emit_for_non_sync_notebook);
  RUN_TEST(test_failed_save_does_not_emit);
  RUN_TEST(test_virtual_buffer_save_does_not_emit);
  RUN_TEST(test_external_buffer_save_does_not_emit);
  RUN_TEST(test_defaults_auto_sync_enabled);
  std::cout << "All buffer-save dirty-tracking tests passed\n";
  return 0;
}
