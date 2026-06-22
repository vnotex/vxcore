// Tests for GitSyncBackend::GetStatus (Task T20 of the git-sync-backend plan).
//
// Each test seeds a bare remote via vxcore_test::create_bare_repo +
// commit_file, drives GitSyncBackend::Initialize down the T15 clone branch
// to populate a notebook root, mutates the working tree (or doesn't, for
// the "clean" case), and asserts on the SyncFileInfo list returned by
// GetStatus.
//
// Compiles git_sync_backend.cpp directly (matches test_git_sync_init) so the
// binary owns its own copy of the GitSyncBackend statics independent of
// vxcore.dll.

#include <git2.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "sync/git/git_sync_backend.h"
#include "sync/git/git_sync_pipeline.h"
#include "sync/git/libgit2_init.h"
#include "sync/sync_types.h"
#include "test_git_sync_helpers.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore_types.h"

namespace {

bool has_path_with_status(const std::vector<vxcore::SyncFileInfo> &files,
                          const std::string &path,
                          vxcore::SyncFileStatus status) {
  for (const auto &f : files) {
    if (f.path == path && f.status == status) {
      return true;
    }
  }
  return false;
}

bool has_any_path_starting_with(const std::vector<vxcore::SyncFileInfo> &files,
                                const std::string &prefix) {
  for (const auto &f : files) {
    if (f.path.compare(0, prefix.size(), prefix) == 0) {
      return true;
    }
  }
  return false;
}

void write_file_at(const std::string &abs_path, const std::string &content) {
  std::filesystem::create_directories(
      vxcore::PathFromUtf8(abs_path).parent_path());
  std::ofstream ofs(vxcore::PathFromUtf8(abs_path),
                    std::ios::binary | std::ios::trunc);
  ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
}

int test_git_get_status_modified_file() {
  std::cout << "  Running test_git_get_status_modified_file..." << std::endl;

  const std::string bare_path = get_test_path("git_status_mod_bare");
  const std::string notebook_root = get_test_path("git_status_mod_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "hello", "init");

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.auto_sync_enabled = true;

    vxcore::GitSyncBackend backend;
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    // Modify the cloned file.
    write_file_at(notebook_root + "/note.md", "hello world");

    std::vector<vxcore::SyncFileInfo> files;
    ASSERT_EQ(backend.GetStatus(files), VXCORE_OK);
    ASSERT_TRUE(has_path_with_status(files, "note.md",
                                     vxcore::SyncFileStatus::kModifiedLocal));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_get_status_modified_file passed" << std::endl;
  return 0;
}

int test_git_get_status_new_file() {
  std::cout << "  Running test_git_get_status_new_file..." << std::endl;

  const std::string bare_path = get_test_path("git_status_new_bare");
  const std::string notebook_root = get_test_path("git_status_new_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "hello", "init");

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.auto_sync_enabled = true;

    vxcore::GitSyncBackend backend;
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    // Add a brand-new untracked file.
    write_file_at(notebook_root + "/fresh.md", "fresh body");

    std::vector<vxcore::SyncFileInfo> files;
    ASSERT_EQ(backend.GetStatus(files), VXCORE_OK);
    ASSERT_TRUE(has_path_with_status(files, "fresh.md",
                                     vxcore::SyncFileStatus::kAddedLocal));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_get_status_new_file passed" << std::endl;
  return 0;
}

int test_git_get_status_clean() {
  std::cout << "  Running test_git_get_status_clean..." << std::endl;

  const std::string bare_path = get_test_path("git_status_clean_bare");
  const std::string notebook_root = get_test_path("git_status_clean_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "hello", "init");

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.auto_sync_enabled = true;

    vxcore::GitSyncBackend backend;
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    // Initialize wrote .gitignore + .gitattributes (untracked). Commit them
    // so the workdir is genuinely clean before we check status.
    auto pipeline = backend.MakePipeline();
    ASSERT_NOT_NULL(pipeline.get());
    ASSERT_EQ(pipeline->StageAll(), VXCORE_OK);
    ASSERT_EQ(pipeline->CommitIndex("vnote: commit defaults"), VXCORE_OK);

    std::vector<vxcore::SyncFileInfo> files;
    ASSERT_EQ(backend.GetStatus(files), VXCORE_OK);
    ASSERT_TRUE(files.empty());
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_get_status_clean passed" << std::endl;
  return 0;
}

// Even when a stray file appears under vx_notebook/vx_sync/ (our gitdir),
// GetStatus must NOT report it — the default .gitignore written during
// Initialize excludes the gitdir.
int test_git_get_status_excludes_vx_sync() {
  std::cout << "  Running test_git_get_status_excludes_vx_sync..." << std::endl;

  const std::string bare_path = get_test_path("git_status_excl_bare");
  const std::string notebook_root = get_test_path("git_status_excl_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "hello", "init");

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.auto_sync_enabled = true;

    vxcore::GitSyncBackend backend;
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    // Drop a stray file inside the gitdir (where libgit2 keeps its own state).
    write_file_at(notebook_root + "/vx_notebook/vx_sync/stray.txt",
                  "should be ignored");

    std::vector<vxcore::SyncFileInfo> files;
    ASSERT_EQ(backend.GetStatus(files), VXCORE_OK);
    ASSERT_FALSE(has_any_path_starting_with(files, "vx_notebook/vx_sync"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_get_status_excludes_vx_sync passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  RUN_TEST(test_git_get_status_modified_file);
  RUN_TEST(test_git_get_status_new_file);
  RUN_TEST(test_git_get_status_clean);
  RUN_TEST(test_git_get_status_excludes_vx_sync);
  std::cout << "All git_sync_status tests passed" << std::endl;
  return 0;
}
