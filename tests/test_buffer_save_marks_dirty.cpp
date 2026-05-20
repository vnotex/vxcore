// T10 of auto-sync-on-save plan: verify that vxcore_buffer_save fires
// `file.saved` events through BufferManager → EventManager → SyncManager and
// marks the parent notebook dirty (which then enqueues a sync job on the
// "sync" WorkQueue). Subtests cover the happy path plus all "should NOT emit"
// edge cases: non-sync notebook, failed save, virtual buffer, external buffer.
// Subtest 6 locks in the new 60-second defaults for SyncConfig::interval_seconds
// and NotebookConfig::sync_interval_seconds (T4/T5 of the plan).
//
// Structural template: test_sync_manager_dispatch.cpp (MockSyncBackend +
// RegisterBackendForTesting + reinterpret_cast<VxCoreContext *> pattern).
// Assertion patterns: test_event_manager.cpp:419-607 (dirty-tracking gold
// standard via GetDirtyNotebooks()).

#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/context.h"
#include "core/work_queue.h"
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

}  // namespace

// ----------------------------------------------------------------------------
// Subtest 1: happy path — save on a sync-enabled notebook marks dirty AND
// enqueues a sync job on the "sync" WorkQueue.
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
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}"),
            VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);

  // Pre-condition: no dirty notebooks, no "sync" queue yet.
  ASSERT_TRUE(ctx_impl->sync_manager->GetDirtyNotebooks().empty());
  ASSERT_TRUE(ctx_impl->work_queue_manager->Get("sync") == nullptr);

  // Create a file in the notebook root.
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "save_emit.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  // Open a buffer for the file, write content, save.
  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, notebook_id, "save_emit.md", &buffer_id), VXCORE_OK);
  ASSERT_EQ(WriteBufferContent(ctx, buffer_id, "hello world"), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(ctx, buffer_id), VXCORE_OK);

  // Assert: notebook is now in the dirty set.
  auto dirty = ctx_impl->sync_manager->GetDirtyNotebooks();
  ASSERT_EQ(dirty.size(), 1u);
  ASSERT_EQ(dirty[0], std::string(notebook_id));

  // Assert: a sync job has been enqueued on the "sync" WorkQueue.
  auto *sync_queue = ctx_impl->work_queue_manager->Get("sync");
  ASSERT_TRUE(sync_queue != nullptr);
  ASSERT_EQ(sync_queue->Size(), static_cast<size_t>(1));

  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  \xE2\x9C\x93 test_save_emits_for_sync_enabled_notebook passed" << std::endl;
  return 0;
}

// ----------------------------------------------------------------------------
// Subtest 2: save on a notebook WITHOUT sync enabled does NOT mark dirty and
// does NOT create a sync queue.
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

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "nosync.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, notebook_id, "nosync.md", &buffer_id), VXCORE_OK);
  ASSERT_EQ(WriteBufferContent(ctx, buffer_id, "no sync content"), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(ctx, buffer_id), VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ASSERT_TRUE(ctx_impl->sync_manager->GetDirtyNotebooks().empty());

  // Either no "sync" queue exists, or it exists but is empty.
  auto *sync_queue = ctx_impl->work_queue_manager->Get("sync");
  if (sync_queue != nullptr) {
    ASSERT_EQ(sync_queue->Size(), static_cast<size_t>(0));
  }

  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  \xE2\x9C\x93 test_save_no_emit_for_non_sync_notebook passed" << std::endl;
  return 0;
}

// ----------------------------------------------------------------------------
// Subtest 3: a failed save (VXCORE_ERR_IO) MUST NOT emit file.saved.
// Force the failure by replacing the on-disk file with a directory of the
// same name before calling save — ofstream::open then fails because the path
// is a directory, BufferManager::SaveBuffer returns VXCORE_ERR_IO before
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
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}"),
            VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "willfail.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);

  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, notebook_id, "willfail.md", &buffer_id), VXCORE_OK);
  ASSERT_EQ(WriteBufferContent(ctx, buffer_id, "content that will fail to save"), VXCORE_OK);

  // Clear dirty state and sync queue from file creation event before testing
  // save failure.
  ctx_impl->sync_manager->ClearDirty(notebook_id);
  ASSERT_TRUE(ctx_impl->sync_manager->GetDirtyNotebooks().empty());
  auto *sync_queue = ctx_impl->work_queue_manager->Get("sync");
  if (sync_queue != nullptr) {
    // Drain the queue that was populated by file creation event
    while (sync_queue->Size() > 0) {
      sync_queue->ProcessNext(0);
    }
  }

  // Replace the file with a directory at the same path so ofstream::open
  // fails. This works cross-platform.
  std::string file_full_path = nb_path + "/willfail.md";
  std::error_code ec;
  std::filesystem::remove(utf8_to_fs_path(file_full_path), ec);
  std::filesystem::create_directory(utf8_to_fs_path(file_full_path), ec);

  // Save must fail with VXCORE_ERR_IO and must NOT emit.
  ASSERT_EQ(vxcore_buffer_save(ctx, buffer_id), VXCORE_ERR_IO);

  // After failed save, dirty state must remain empty (no event emitted).
  ASSERT_TRUE(ctx_impl->sync_manager->GetDirtyNotebooks().empty());
  if (sync_queue != nullptr) {
    ASSERT_EQ(sync_queue->Size(), static_cast<size_t>(0));
  }

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
                               "{\"backend\":\"mock\",\"remoteUrl\":\"file:///tmp/x\"}"),
            VXCORE_OK);

  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);

  // Open a virtual buffer (no filesystem backing) and save it.
  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open_virtual(ctx, "vx://settings", &buffer_id), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(ctx, buffer_id), VXCORE_OK);

  // No emit: dirty set is empty, no sync job enqueued.
  ASSERT_TRUE(ctx_impl->sync_manager->GetDirtyNotebooks().empty());
  auto *sync_queue = ctx_impl->work_queue_manager->Get("sync");
  if (sync_queue != nullptr) {
    ASSERT_EQ(sync_queue->Size(), static_cast<size_t>(0));
  }

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
// saving a standalone file does not mark any notebook dirty.
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

  // Open a buffer for a standalone file outside any notebook.
  // Pass nullptr for notebook_id (the API treats it as "" — external file).
  std::string standalone_file = ext_dir + "/standalone.md";
  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, nullptr, standalone_file.c_str(), &buffer_id), VXCORE_OK);
  ASSERT_EQ(WriteBufferContent(ctx, buffer_id, "external content"), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(ctx, buffer_id), VXCORE_OK);

  // No notebook id → no emit → no dirty notebook.
  auto *ctx_impl = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  ASSERT_TRUE(ctx_impl->sync_manager->GetDirtyNotebooks().empty());
  auto *sync_queue = ctx_impl->work_queue_manager->Get("sync");
  if (sync_queue != nullptr) {
    ASSERT_EQ(sync_queue->Size(), static_cast<size_t>(0));
  }

  vxcore_string_free(buffer_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(ext_dir);
  std::cout << "  \xE2\x9C\x93 test_external_buffer_save_does_not_emit passed" << std::endl;
  return 0;
}

// ----------------------------------------------------------------------------
// Subtest 6: lock in the new 60-second defaults (T4: SyncConfig,
// T5: NotebookConfig). SyncConfig is tested directly because its FromJson +
// ctor are header-inlined. NotebookConfig is tested via the C API
// (vxcore_notebook_create + vxcore_notebook_get_config) because its ctor +
// FromJson are non-exported out-of-line symbols inside vxcore.dll.
// ----------------------------------------------------------------------------
int test_defaults_are_60() {
  std::cout << "  Running test_defaults_are_60..." << std::endl;

  // Default-constructed SyncConfig — interval_seconds defaults to 60.
  SyncConfig cfg_default;
  ASSERT_EQ(cfg_default.interval_seconds, 60);

  // FromJson with no intervalSeconds key preserves the default.
  nlohmann::json sync_json = nlohmann::json::parse("{\"backend\":\"git\"}");
  SyncConfig cfg_from_json = SyncConfig::FromJson(sync_json);
  ASSERT_EQ(cfg_from_json.interval_seconds, 60);

  // NotebookConfig default — verified via the C API to avoid linking against
  // non-exported symbols.
  std::string nb_path = get_test_path("test_defaults_nb");
  cleanup_test_dir(nb_path);
  vxcore_set_test_mode(1);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *notebook_id = nullptr;
  // Create notebook with only "name" set — no syncIntervalSeconds key.
  ASSERT_EQ(vxcore_notebook_create(ctx, nb_path.c_str(), "{\"name\":\"DefaultsTest\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);

  char *config_json = nullptr;
  ASSERT_EQ(vxcore_notebook_get_config(ctx, notebook_id, &config_json), VXCORE_OK);
  nlohmann::json config = nlohmann::json::parse(config_json);
  ASSERT_TRUE(config.contains("syncIntervalSeconds"));
  ASSERT_TRUE(config["syncIntervalSeconds"].is_number_integer());
  ASSERT_EQ(config["syncIntervalSeconds"].get<int>(), 60);

  vxcore_string_free(config_json);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(nb_path);
  std::cout << "  \xE2\x9C\x93 test_defaults_are_60 passed" << std::endl;
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
  RUN_TEST(test_defaults_are_60);
  std::cout << "All buffer-save dirty-tracking tests passed\n";
  return 0;
}
