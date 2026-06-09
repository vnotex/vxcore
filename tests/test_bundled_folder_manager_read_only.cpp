// T9 of open-notebook-remote-readonly plan.
//
// Verifies that EVERY public mutating method on BundledFolderManager honors
// the read-only guard: when Notebook::IsReadOnly() returns true the mutator
// must return VXCORE_ERR_READ_ONLY and leave on-disk state unchanged. The
// happy-path subtest re-enables the notebook and asserts the same mutators
// still succeed (sanity / no regression).
//
// Source guards live at the TOP of each method in
// libs/vxcore/src/core/bundled_folder_manager.cpp (21 guarded mutators,
// committed in vxcore b7f13c5).
//
// Test isolation note: every vxcore test shares
// %TEMP%/vxcore_test_data + %TEMP%/vxcore_test_config (see
// libs/vxcore/AGENTS.md). Creating a new context per subtest can corrupt
// session.json across the sequence (observed: 5th vxcore_context_create
// returned VXCORE_ERR_INVALID_JSON). To avoid that fragility we follow the
// existing T9 pattern in tests/test_folder.cpp: ONE shared context for the
// whole binary, with each subtest creating its own notebook so disk state
// remains isolated.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace {

// Globals shared across all subtests — initialized once in main(), torn down
// after the suite completes.
VxCoreContextHandle g_ctx = nullptr;

// Count direct entries (files + folders) in a directory, skipping any name
// starting with "vx_" (skips vx_notebook bookkeeping so the test only
// observes user-visible state).
size_t CountUserEntries(const std::string &dir_utf8) {
  size_t n = 0;
  std::error_code ec;
  auto fs_dir = utf8_to_fs_path(dir_utf8);
  if (!std::filesystem::exists(fs_dir, ec)) {
    return 0;
  }
  for (auto it = std::filesystem::directory_iterator(fs_dir, ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    const auto name = fs_path_to_utf8(it->path().filename());
    if (name.rfind("vx_", 0) == 0) {
      continue;
    }
    ++n;
  }
  return n;
}

// Read a file's raw bytes; returns "" on any failure.
std::string ReadFileBytes(const std::string &path_utf8) {
  std::ifstream fs(utf8_to_fs_path(path_utf8), std::ios::binary);
  if (!fs.is_open()) {
    return {};
  }
  std::string out((std::istreambuf_iterator<char>(fs)), std::istreambuf_iterator<char>());
  return out;
}

// Build a fresh writable bundled notebook on disk. Caller owns the returned
// notebook id (free with vxcore_string_free) AND the on-disk directory
// (clean up at end of subtest via cleanup_test_dir).
VxCoreError CreateWritableNotebook(const std::string &name, std::string &out_nb_path,
                                   char **out_notebook_id) {
  out_nb_path = get_test_path(name);
  cleanup_test_dir(out_nb_path);
  create_directory(out_nb_path);
  return vxcore_notebook_create(g_ctx, out_nb_path.c_str(), R"({"name":"T9 BundledRO"})",
                                VXCORE_NOTEBOOK_BUNDLED, out_notebook_id);
}

// Seed the notebook with one folder + one file so every mutator test has
// targets to operate on.
int SeedNotebook(const char *notebook_id) {
  char *seed_folder_id = nullptr;
  VxCoreError err = vxcore_folder_create(g_ctx, notebook_id, ".", "seed_folder", &seed_folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(seed_folder_id);

  char *seed_file_id = nullptr;
  err = vxcore_file_create(g_ctx, notebook_id, ".", "seed.md", &seed_file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(seed_file_id);
  return 0;
}

// RAII-ish guard that frees the notebook id and wipes its on-disk root when
// the subtest returns or unwinds.
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

// Subtest 1: CreateFolder is rejected, disk unchanged.
int test_create_folder_rejected_when_read_only() {
  std::cout << "  Running test_create_folder_rejected_when_read_only..." << std::endl;
  NotebookCleanup nb{nullptr, {}};
  ASSERT_EQ(CreateWritableNotebook("t9_create_folder_nb", nb.nb_path, &nb.notebook_id), VXCORE_OK);
  ASSERT_EQ(SeedNotebook(nb.notebook_id), 0);

  const size_t before = CountUserEntries(nb.nb_path);

  ASSERT_EQ(vxcore_notebook_set_read_only(g_ctx, nb.notebook_id, 1), VXCORE_OK);

  char *out_id = nullptr;
  VxCoreError err =
      vxcore_folder_create(g_ctx, nb.notebook_id, ".", "should_not_exist", &out_id);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);
  if (out_id) {
    vxcore_string_free(out_id);
  }

  ASSERT_EQ(CountUserEntries(nb.nb_path), before);
  ASSERT_FALSE(path_exists(nb.nb_path + "/should_not_exist"));
  std::cout << "  \xe2\x9c\x93 test_create_folder_rejected_when_read_only passed" << std::endl;
  return 0;
}

// Subtest 2: CreateFile is rejected, disk unchanged.
int test_create_file_rejected_when_read_only() {
  std::cout << "  Running test_create_file_rejected_when_read_only..." << std::endl;
  NotebookCleanup nb{nullptr, {}};
  ASSERT_EQ(CreateWritableNotebook("t9_create_file_nb", nb.nb_path, &nb.notebook_id), VXCORE_OK);
  ASSERT_EQ(SeedNotebook(nb.notebook_id), 0);

  const size_t before = CountUserEntries(nb.nb_path);

  ASSERT_EQ(vxcore_notebook_set_read_only(g_ctx, nb.notebook_id, 1), VXCORE_OK);

  char *out_id = nullptr;
  VxCoreError err = vxcore_file_create(g_ctx, nb.notebook_id, ".", "blocked.md", &out_id);
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);
  if (out_id) {
    vxcore_string_free(out_id);
  }

  ASSERT_EQ(CountUserEntries(nb.nb_path), before);
  ASSERT_FALSE(path_exists(nb.nb_path + "/blocked.md"));
  std::cout << "  \xe2\x9c\x93 test_create_file_rejected_when_read_only passed" << std::endl;
  return 0;
}

// Subtest 3: DeleteFolder is rejected, seed_folder still on disk.
int test_delete_folder_rejected_when_read_only() {
  std::cout << "  Running test_delete_folder_rejected_when_read_only..." << std::endl;
  NotebookCleanup nb{nullptr, {}};
  ASSERT_EQ(CreateWritableNotebook("t9_delete_folder_nb", nb.nb_path, &nb.notebook_id), VXCORE_OK);
  ASSERT_EQ(SeedNotebook(nb.notebook_id), 0);

  ASSERT_TRUE(path_exists(nb.nb_path + "/seed_folder"));

  ASSERT_EQ(vxcore_notebook_set_read_only(g_ctx, nb.notebook_id, 1), VXCORE_OK);

  VxCoreError err = vxcore_node_delete(g_ctx, nb.notebook_id, "seed_folder");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  ASSERT_TRUE(path_exists(nb.nb_path + "/seed_folder"));
  std::cout << "  \xe2\x9c\x93 test_delete_folder_rejected_when_read_only passed" << std::endl;
  return 0;
}

// Subtest 4: RenameFolder is rejected, original name preserved.
int test_rename_folder_rejected_when_read_only() {
  std::cout << "  Running test_rename_folder_rejected_when_read_only..." << std::endl;
  NotebookCleanup nb{nullptr, {}};
  ASSERT_EQ(CreateWritableNotebook("t9_rename_folder_nb", nb.nb_path, &nb.notebook_id), VXCORE_OK);
  ASSERT_EQ(SeedNotebook(nb.notebook_id), 0);

  ASSERT_TRUE(path_exists(nb.nb_path + "/seed_folder"));

  ASSERT_EQ(vxcore_notebook_set_read_only(g_ctx, nb.notebook_id, 1), VXCORE_OK);

  VxCoreError err =
      vxcore_node_rename(g_ctx, nb.notebook_id, "seed_folder", "renamed_folder");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  ASSERT_TRUE(path_exists(nb.nb_path + "/seed_folder"));
  ASSERT_FALSE(path_exists(nb.nb_path + "/renamed_folder"));
  std::cout << "  \xe2\x9c\x93 test_rename_folder_rejected_when_read_only passed" << std::endl;
  return 0;
}

// Subtest 5: UpdateFileMetadata is rejected, file bytes unchanged.
int test_update_file_metadata_rejected_when_read_only() {
  std::cout << "  Running test_update_file_metadata_rejected_when_read_only..." << std::endl;
  NotebookCleanup nb{nullptr, {}};
  ASSERT_EQ(CreateWritableNotebook("t9_update_meta_nb", nb.nb_path, &nb.notebook_id), VXCORE_OK);
  ASSERT_EQ(SeedNotebook(nb.notebook_id), 0);

  const std::string seed_path = nb.nb_path + "/seed.md";
  ASSERT_TRUE(path_exists(seed_path));
  const std::string before_bytes = ReadFileBytes(seed_path);

  ASSERT_EQ(vxcore_notebook_set_read_only(g_ctx, nb.notebook_id, 1), VXCORE_OK);

  VxCoreError err = vxcore_node_update_metadata(g_ctx, nb.notebook_id, "seed.md",
                                                R"({"tags":["t9-block"]})");
  ASSERT_EQ(err, VXCORE_ERR_READ_ONLY);

  const std::string after_bytes = ReadFileBytes(seed_path);
  ASSERT_EQ(before_bytes, after_bytes);
  std::cout << "  \xe2\x9c\x93 test_update_file_metadata_rejected_when_read_only passed"
            << std::endl;
  return 0;
}

// Subtest 6: Happy path — once read-only is cleared, all five mutators
// succeed on the SAME fixture (in order). This is the "no regression on
// writable notebooks" check the plan requires.
int test_mutators_succeed_when_writable() {
  std::cout << "  Running test_mutators_succeed_when_writable..." << std::endl;
  NotebookCleanup nb{nullptr, {}};
  ASSERT_EQ(CreateWritableNotebook("t9_happy_path_nb", nb.nb_path, &nb.notebook_id), VXCORE_OK);

  // Flip RO on then off to prove the guard releases.
  ASSERT_EQ(vxcore_notebook_set_read_only(g_ctx, nb.notebook_id, 1), VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_set_read_only(g_ctx, nb.notebook_id, 0), VXCORE_OK);

  // CreateFolder.
  char *created_folder_id = nullptr;
  ASSERT_EQ(
      vxcore_folder_create(g_ctx, nb.notebook_id, ".", "writable_folder", &created_folder_id),
      VXCORE_OK);
  ASSERT_NOT_NULL(created_folder_id);
  vxcore_string_free(created_folder_id);
  ASSERT_TRUE(path_exists(nb.nb_path + "/writable_folder"));

  // CreateFile.
  char *created_file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(g_ctx, nb.notebook_id, ".", "writable.md", &created_file_id),
            VXCORE_OK);
  ASSERT_NOT_NULL(created_file_id);
  vxcore_string_free(created_file_id);
  ASSERT_TRUE(path_exists(nb.nb_path + "/writable.md"));

  // UpdateFileMetadata.
  ASSERT_EQ(vxcore_node_update_metadata(g_ctx, nb.notebook_id, "writable.md",
                                        R"({"tags":["t9-ok"]})"),
            VXCORE_OK);

  // RenameFolder.
  ASSERT_EQ(
      vxcore_node_rename(g_ctx, nb.notebook_id, "writable_folder", "renamed_writable_folder"),
      VXCORE_OK);
  ASSERT_FALSE(path_exists(nb.nb_path + "/writable_folder"));
  ASSERT_TRUE(path_exists(nb.nb_path + "/renamed_writable_folder"));

  // DeleteFolder (the renamed one). Bundled FolderManager moves deletes
  // into vx_notebook recycle bin — assert the user-visible path is gone.
  ASSERT_EQ(vxcore_node_delete(g_ctx, nb.notebook_id, "renamed_writable_folder"), VXCORE_OK);
  ASSERT_FALSE(path_exists(nb.nb_path + "/renamed_writable_folder"));

  std::cout << "  \xe2\x9c\x93 test_mutators_succeed_when_writable passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  // Redirect AppData / LocalData to %TEMP%\vxcore_test* per vxcore test
  // conventions (see libs/vxcore/AGENTS.md § Test Mode).
  vxcore_set_test_mode(1);

  // Single shared context for the whole binary — see file-header rationale.
  VxCoreError err = vxcore_context_create(nullptr, &g_ctx);
  if (err != VXCORE_OK) {
    std::cerr << "vxcore_context_create failed: error=" << err << std::endl;
    return 1;
  }

  std::cout << "Running BundledFolderManager read-only guard tests (T9)..." << std::endl;

  RUN_TEST(test_create_folder_rejected_when_read_only);
  RUN_TEST(test_create_file_rejected_when_read_only);
  RUN_TEST(test_delete_folder_rejected_when_read_only);
  RUN_TEST(test_rename_folder_rejected_when_read_only);
  RUN_TEST(test_update_file_metadata_rejected_when_read_only);
  RUN_TEST(test_mutators_succeed_when_writable);

  vxcore_context_destroy(g_ctx);
  g_ctx = nullptr;

  std::cout << "\xe2\x9c\x93 All BundledFolderManager read-only guard tests passed" << std::endl;
  return 0;
}
