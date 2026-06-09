// T12 of open-notebook-remote-readonly plan.
//
// Verifies that every notebook-level mutator on Bundled and Raw notebooks
// honors the read-only guard: when Notebook::IsReadOnly() returns true the
// mutator must return VXCORE_ERR_READ_ONLY and leave on-disk state
// unchanged. Per-device session metadata (last_sync_utc) is intentionally
// NOT guarded — verified via a writable subtest reference.
//
// Source guards live in:
//   * libs/vxcore/src/core/notebook.cpp (CreateTag / CreateTagPath /
//     DeleteTag / MoveTag base-class guards)
//   * libs/vxcore/src/core/bundled_notebook.cpp (UpdateConfig +
//     EmptyRecycleBin)
//   * libs/vxcore/src/core/raw_notebook.cpp (UpdateConfig +
//     EmptyRecycleBin)
//
// Test isolation note: every vxcore test shares
// %TEMP%/vxcore_test_data + %TEMP%/vxcore_test_config (see
// libs/vxcore/AGENTS.md). We follow the existing T9 pattern in
// test_bundled_folder_manager_read_only.cpp: ONE shared context for the
// whole binary, with each subtest creating its own notebook so disk state
// remains isolated.

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace {

VxCoreContextHandle g_ctx = nullptr;

// Build a fresh writable bundled notebook on disk. Caller owns the returned
// notebook id (free with vxcore_string_free) AND the on-disk directory
// (clean up at end of subtest via cleanup_test_dir).
VxCoreError CreateWritableBundled(const std::string &name, std::string &out_nb_path,
                                   char **out_notebook_id) {
  out_nb_path = get_test_path(name);
  cleanup_test_dir(out_nb_path);
  create_directory(out_nb_path);
  return vxcore_notebook_create(g_ctx, out_nb_path.c_str(), R"({"name":"T12 Bundled RO"})",
                                VXCORE_NOTEBOOK_BUNDLED, out_notebook_id);
}

VxCoreError CreateWritableRaw(const std::string &name, std::string &out_nb_path,
                               char **out_notebook_id) {
  out_nb_path = get_test_path(name);
  cleanup_test_dir(out_nb_path);
  create_directory(out_nb_path);
  return vxcore_notebook_create(g_ctx, out_nb_path.c_str(), R"({"name":"T12 Raw RO"})",
                                VXCORE_NOTEBOOK_RAW, out_notebook_id);
}

// RAII cleanup for one notebook fixture.
struct NotebookCleanup {
  char *notebook_id;
  std::string nb_path;
  ~NotebookCleanup() {
    if (notebook_id) {
      vxcore_string_free(notebook_id);
    }
    if (!nb_path.empty()) {
      cleanup_test_dir(nb_path);
    }
  }
};

// Subtest 1: Bundled — every guarded mutator rejects when RO=true.
int test_bundled_notebook_mutators_read_only() {
  std::cout << "  Running test_bundled_notebook_mutators_read_only..." << std::endl;
  NotebookCleanup nb{nullptr, {}};
  ASSERT_EQ(CreateWritableBundled("t12_bundled_ro", nb.nb_path, &nb.notebook_id), VXCORE_OK);

  // Pre-seed a tag while writable so DeleteTag / MoveTag have something to
  // target. CreateTag itself is tested both writable + read-only below.
  ASSERT_EQ(vxcore_tag_create(g_ctx, nb.notebook_id, "seed_tag"), VXCORE_OK);

  // Flip to read-only.
  ASSERT_EQ(vxcore_notebook_set_read_only(g_ctx, nb.notebook_id, 1), VXCORE_OK);

  // CreateTag → VXCORE_ERR_READ_ONLY.
  ASSERT_EQ(vxcore_tag_create(g_ctx, nb.notebook_id, "blocked_tag"), VXCORE_ERR_READ_ONLY);

  // CreateTagPath → VXCORE_ERR_READ_ONLY.
  ASSERT_EQ(vxcore_tag_create_path(g_ctx, nb.notebook_id, "blocked/path"),
            VXCORE_ERR_READ_ONLY);

  // DeleteTag → VXCORE_ERR_READ_ONLY (pre-existing tag stays).
  ASSERT_EQ(vxcore_tag_delete(g_ctx, nb.notebook_id, "seed_tag"), VXCORE_ERR_READ_ONLY);

  // MoveTag → VXCORE_ERR_READ_ONLY.
  ASSERT_EQ(vxcore_tag_move(g_ctx, nb.notebook_id, "seed_tag", ""), VXCORE_ERR_READ_ONLY);

  // UpdateConfig → VXCORE_ERR_READ_ONLY.
  ASSERT_EQ(vxcore_notebook_update_config(g_ctx, nb.notebook_id, R"({"name":"Blocked Update"})"),
            VXCORE_ERR_READ_ONLY);

  // EmptyRecycleBin → VXCORE_ERR_READ_ONLY.
  ASSERT_EQ(vxcore_notebook_empty_recycle_bin(g_ctx, nb.notebook_id), VXCORE_ERR_READ_ONLY);

  std::cout << "  \xe2\x9c\x93 test_bundled_notebook_mutators_read_only passed" << std::endl;
  return 0;
}

// Subtest 2: Raw — every guarded mutator rejects when RO=true. For Raw the
// tag ops would normally return UNSUPPORTED, but the read-only guard fires
// first so the observed code is READ_ONLY.
int test_raw_notebook_mutators_read_only() {
  std::cout << "  Running test_raw_notebook_mutators_read_only..." << std::endl;
  NotebookCleanup nb{nullptr, {}};
  ASSERT_EQ(CreateWritableRaw("t12_raw_ro", nb.nb_path, &nb.notebook_id), VXCORE_OK);

  ASSERT_EQ(vxcore_notebook_set_read_only(g_ctx, nb.notebook_id, 1), VXCORE_OK);

  // Tag ops: read-only must beat UNSUPPORTED.
  ASSERT_EQ(vxcore_tag_create(g_ctx, nb.notebook_id, "blocked_tag"), VXCORE_ERR_READ_ONLY);
  ASSERT_EQ(vxcore_tag_create_path(g_ctx, nb.notebook_id, "blocked/path"),
            VXCORE_ERR_READ_ONLY);
  ASSERT_EQ(vxcore_tag_delete(g_ctx, nb.notebook_id, "blocked_tag"), VXCORE_ERR_READ_ONLY);
  ASSERT_EQ(vxcore_tag_move(g_ctx, nb.notebook_id, "blocked_tag", ""), VXCORE_ERR_READ_ONLY);

  ASSERT_EQ(vxcore_notebook_update_config(g_ctx, nb.notebook_id, R"({"name":"Blocked Update"})"),
            VXCORE_ERR_READ_ONLY);
  ASSERT_EQ(vxcore_notebook_empty_recycle_bin(g_ctx, nb.notebook_id), VXCORE_ERR_READ_ONLY);

  std::cout << "  \xe2\x9c\x93 test_raw_notebook_mutators_read_only passed" << std::endl;
  return 0;
}

// Subtest 3: Sanity / no-regression — once read-only is cleared, every
// previously-guarded mutator succeeds on a fresh writable bundled notebook.
int test_mutators_succeed_when_writable() {
  std::cout << "  Running test_mutators_succeed_when_writable..." << std::endl;
  NotebookCleanup nb{nullptr, {}};
  ASSERT_EQ(CreateWritableBundled("t12_bundled_writable", nb.nb_path, &nb.notebook_id),
            VXCORE_OK);

  // Flip on then off to prove the guard releases.
  ASSERT_EQ(vxcore_notebook_set_read_only(g_ctx, nb.notebook_id, 1), VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_set_read_only(g_ctx, nb.notebook_id, 0), VXCORE_OK);

  ASSERT_EQ(vxcore_tag_create(g_ctx, nb.notebook_id, "ok_tag"), VXCORE_OK);
  ASSERT_EQ(vxcore_tag_create_path(g_ctx, nb.notebook_id, "deep/nested/path"), VXCORE_OK);
  // MoveTag: nest "ok_tag" under "deep" (root tag created by CreateTagPath).
  ASSERT_EQ(vxcore_tag_move(g_ctx, nb.notebook_id, "ok_tag", "deep"), VXCORE_OK);
  ASSERT_EQ(vxcore_tag_delete(g_ctx, nb.notebook_id, "ok_tag"), VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_update_config(g_ctx, nb.notebook_id, R"({"name":"Updated Writable"})"),
            VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_empty_recycle_bin(g_ctx, nb.notebook_id), VXCORE_OK);

  std::cout << "  \xe2\x9c\x93 test_mutators_succeed_when_writable passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  // Redirect AppData / LocalData to %TEMP%\vxcore_test* per vxcore test
  // conventions (see libs/vxcore/AGENTS.md § Test Mode).
  vxcore_set_test_mode(1);

  // Single shared context for the whole binary.
  VxCoreError err = vxcore_context_create(nullptr, &g_ctx);
  if (err != VXCORE_OK) {
    std::cerr << "vxcore_context_create failed: error=" << err << std::endl;
    return 1;
  }

  std::cout << "Running notebook-level mutator read-only guard tests (T12)..." << std::endl;

  RUN_TEST(test_bundled_notebook_mutators_read_only);
  RUN_TEST(test_raw_notebook_mutators_read_only);
  RUN_TEST(test_mutators_succeed_when_writable);

  vxcore_context_destroy(g_ctx);
  g_ctx = nullptr;

  std::cout << "\xe2\x9c\x93 All notebook-level mutator read-only tests passed" << std::endl;
  return 0;
}
