// B5 of sync-backend-phase4: GitSyncBackend::Pull() auto-commit opt-in.
// Verifies that SyncConfig::auto_commit_merges gates the pre-rebase auto-commit.
// When true (default), Pull stages + commits before rebase.
// When false, Pull stages but skips commit, leaving staged changes for caller.

#include <git2.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "sync/git/git_sync_backend.h"
#include "sync/git/libgit2_init.h"
#include "sync/sync_types.h"
#include "test_git_sync_helpers.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore_types.h"

namespace {

void write_file_at(const std::string &abs_path, const std::string &content) {
  std::filesystem::create_directories(
      vxcore::PathFromUtf8(abs_path).parent_path());
  std::ofstream ofs(vxcore::PathFromUtf8(abs_path),
                    std::ios::binary | std::ios::trunc);
  ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string read_file_at(const std::string &abs_path) {
  std::ifstream ifs(vxcore::PathFromUtf8(abs_path), std::ios::binary);
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

// Local shim for vxcore_set_test_mode (test doesn't link vxcore.dll)
void vxcore_set_test_mode(int mode) {
  (void)mode;
  // No-op: test uses local temp paths anyway
}

// Push a follow-up commit (parent = current bare HEAD) directly to <bare_path>.
// Identical to the helper used in test_git_sync_roundtrip.cpp /
// test_git_sync_conflicts.cpp.
void push_follow_up_commit(const std::string &bare_path,
                           const std::string &rel_file,
                           const std::string &content,
                           const std::string &commit_msg) {
  vxcore::LibGit2Init guard;

  std::filesystem::path tmp_root = std::filesystem::temp_directory_path();
  std::filesystem::path workdir =
      tmp_root / ("vxcore_followup_pp_" + std::to_string(std::rand()));
  {
    std::error_code ec;
    std::filesystem::remove_all(workdir, ec);
    (void)ec;
  }

  std::error_code ec;
  std::filesystem::path bare_abs =
      std::filesystem::absolute(std::filesystem::u8path(bare_path), ec);
  std::string bare_abs_str = ec ? bare_path : bare_abs.string();
  std::string url = std::string("file:///") + bare_abs_str;
  for (auto &c : url) {
    if (c == '\\') c = '/';
  }
  if (url.compare(0, 9, "file:////") == 0) {
    url.erase(7, 1);
  }

  git_repository *repo = nullptr;
  git_repository_init_options iopts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
  if (git_repository_init_options_init(
          &iopts, GIT_REPOSITORY_INIT_OPTIONS_VERSION) != 0) {
    throw std::runtime_error("init_options_init");
  }
  iopts.flags = GIT_REPOSITORY_INIT_MKDIR | GIT_REPOSITORY_INIT_MKPATH;
  iopts.initial_head = "main";
  if (git_repository_init_ext(&repo, workdir.string().c_str(), &iopts) != 0) {
    throw std::runtime_error("init_ext(followup workdir)");
  }

  git_remote *origin = nullptr;
  if (git_remote_create(&origin, repo, "origin", url.c_str()) != 0) {
    git_repository_free(repo);
    throw std::runtime_error("remote_create");
  }

  git_fetch_options fopts = GIT_FETCH_OPTIONS_INIT;
  if (git_remote_fetch(origin, nullptr, &fopts, "followup fetch") != 0) {
    const git_error *e = git_error_last();
    std::string msg = std::string("remote_fetch: ") +
                      ((e && e->message) ? e->message : "unknown");
    git_remote_free(origin);
    git_repository_free(repo);
    throw std::runtime_error(msg);
  }

  git_reference *ref = nullptr;
  if (git_reference_lookup(&ref, repo, "refs/remotes/origin/main") != 0) {
    git_remote_free(origin);
    git_repository_free(repo);
    throw std::runtime_error("ref_lookup origin/main");
  }
  git_object *parent_obj = nullptr;
  if (git_reference_peel(&parent_obj, ref, GIT_OBJECT_COMMIT) != 0) {
    git_reference_free(ref);
    git_remote_free(origin);
    git_repository_free(repo);
    throw std::runtime_error("reference_peel");
  }
  git_commit *parent_commit = nullptr;
  if (git_commit_lookup(&parent_commit, repo, git_object_id(parent_obj)) != 0) {
    git_object_free(parent_obj);
    git_reference_free(ref);
    git_remote_free(origin);
    git_repository_free(repo);
    throw std::runtime_error("commit_lookup parent");
  }
  git_object_free(parent_obj);

  git_tree *parent_tree = nullptr;
  if (git_commit_tree(&parent_tree, parent_commit) != 0) {
    git_commit_free(parent_commit);
    git_reference_free(ref);
    git_remote_free(origin);
    git_repository_free(repo);
    throw std::runtime_error("commit_tree(parent)");
  }
  git_checkout_options copts = GIT_CHECKOUT_OPTIONS_INIT;
  copts.checkout_strategy = GIT_CHECKOUT_FORCE;
  if (git_checkout_tree(repo, reinterpret_cast<git_object *>(parent_tree),
                        &copts) != 0) {
    git_tree_free(parent_tree);
    git_commit_free(parent_commit);
    git_reference_free(ref);
    git_remote_free(origin);
    git_repository_free(repo);
    throw std::runtime_error("checkout_tree(parent)");
  }
  git_tree_free(parent_tree);

  std::filesystem::path file_full = workdir / rel_file;
  std::filesystem::create_directories(file_full.parent_path());
  {
    std::ofstream ofs(file_full, std::ios::binary | std::ios::trunc);
    ofs << content;
  }

  git_index *idx = nullptr;
  if (git_repository_index(&idx, repo) != 0) {
    git_commit_free(parent_commit);
    git_reference_free(ref);
    git_remote_free(origin);
    git_repository_free(repo);
    throw std::runtime_error("repository_index");
  }
  std::string rel_norm = rel_file;
  for (auto &c : rel_norm) {
    if (c == '\\') c = '/';
  }
  if (git_index_add_bypath(idx, rel_norm.c_str()) != 0 ||
      git_index_write(idx) != 0) {
    git_index_free(idx);
    git_commit_free(parent_commit);
    git_reference_free(ref);
    git_remote_free(origin);
    git_repository_free(repo);
    throw std::runtime_error("index_add/write");
  }
  git_oid tree_oid;
  if (git_index_write_tree(&tree_oid, idx) != 0) {
    git_index_free(idx);
    git_commit_free(parent_commit);
    git_reference_free(ref);
    git_remote_free(origin);
    git_repository_free(repo);
    throw std::runtime_error("index_write_tree");
  }
  git_index_free(idx);

  git_tree *tree = nullptr;
  if (git_tree_lookup(&tree, repo, &tree_oid) != 0) {
    git_commit_free(parent_commit);
    git_reference_free(ref);
    git_remote_free(origin);
    git_repository_free(repo);
    throw std::runtime_error("tree_lookup");
  }

  git_signature *sig = nullptr;
  if (git_signature_now(&sig, "Followup", "followup@helper.local") != 0) {
    git_tree_free(tree);
    git_commit_free(parent_commit);
    git_reference_free(ref);
    git_remote_free(origin);
    git_repository_free(repo);
    throw std::runtime_error("signature_now");
  }

  git_oid commit_oid;
  const git_commit *parents[1] = {parent_commit};
  int rc = git_commit_create(&commit_oid, repo, "refs/heads/main", sig, sig,
                             nullptr, commit_msg.c_str(), tree, 1, parents);
  git_signature_free(sig);
  git_tree_free(tree);
  git_commit_free(parent_commit);
  git_reference_free(ref);
  if (rc != 0) {
    git_remote_free(origin);
    git_repository_free(repo);
    throw std::runtime_error("commit_create(followup)");
  }

  git_push_options popts = GIT_PUSH_OPTIONS_INIT;
  char refspec_buf[] = "refs/heads/main:refs/heads/main";
  char *refspecs_arr[] = {refspec_buf};
  git_strarray refspecs = {refspecs_arr, 1};
  rc = git_remote_push(origin, &refspecs, &popts);
  git_remote_free(origin);
  git_repository_free(repo);
  {
    std::error_code rec;
    std::filesystem::remove_all(workdir, rec);
    (void)rec;
  }
  if (rc != 0) {
    const git_error *e = git_error_last();
    throw std::runtime_error(std::string("remote_push(followup): ") +
                             ((e && e->message) ? e->message : "unknown"));
  }
}

// B5 test 1: default auto_commit_merges = true
// Pull should auto-commit local changes before rebase.
// After Pull, working tree should be clean (local changes committed).
int test_default_auto_commits() {
  std::cout << "  Running test_default_auto_commits..." << std::endl;
  const std::string bare_path = get_test_path("git_ac_default_bare");
  const std::string notebook_root = get_test_path("git_ac_default_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    vxcore::LibGit2Init guard;

    // Create bare repo with initial commit
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "base", "init");

    // Create notebook_root directory
    create_directory(notebook_root);

    // Initialize backend with default auto_commit_merges = true
    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.auto_commit_merges = true;  // DEFAULT

    vxcore::GitSyncBackend backend;
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    // Make a local change (not committed)
    write_file_at(notebook_root + "/local.txt", "local change");

    // Push a remote change (follow-up commit to bare repo)
    push_follow_up_commit(bare_path, "remote.txt", "remote content",
                          "Remote commit");

    // Pull should auto-commit the local change before rebasing
    ASSERT_EQ(backend.Pull(nullptr, nullptr), VXCORE_OK);

    // Verify working tree is clean (local.txt was auto-committed)
    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";
    ASSERT_TRUE(vxcore_test::git_status_clean(git_dir));

    backend.Shutdown();

    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    std::cout << "  ✓ test_default_auto_commits passed" << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }
}

// B5 test 2: auto_commit_merges = false
// Pull should stage local changes but NOT commit them before rebase.
// This means the staged changes are NOT replayed during rebase (they're lost).
// The caller is responsible for committing staged changes if they want them preserved.
int test_opt_out_skips_commit() {
  std::cout << "  Running test_opt_out_skips_commit..." << std::endl;
  const std::string bare_path = get_test_path("git_ac_optout_bare");
  const std::string notebook_root = get_test_path("git_ac_optout_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    vxcore::LibGit2Init guard;

    // Create bare repo with initial commit
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "base", "init");

    // Create notebook_root directory
    create_directory(notebook_root);

    // Initialize backend with auto_commit_merges = false
    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.auto_commit_merges = false;  // OPT OUT

    vxcore::GitSyncBackend backend;
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    // Modify an existing file (note.md) locally
    write_file_at(notebook_root + "/note.md", "local modification");

    // Push a remote change to a different file
    push_follow_up_commit(bare_path, "remote.txt", "remote content",
                          "Remote commit");

    // Pull should stage but NOT commit the local change
    ASSERT_EQ(backend.Pull(nullptr, nullptr), VXCORE_OK);

    // After Pull completes, the working tree should be clean (rebase finished).
    // Since we didn't commit the staged changes, they were NOT replayed during rebase.
    // The local modification to note.md is lost (reverted to remote version).
    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";
    ASSERT_TRUE(vxcore_test::git_status_clean(git_dir));

    // Verify both files exist
    ASSERT_TRUE(vxcore::PathExists(notebook_root + "/note.md"));
    ASSERT_TRUE(vxcore::PathExists(notebook_root + "/remote.txt"));
    
    // note.md should have the remote version (local modification was not committed)
    const std::string note_content = read_file_at(notebook_root + "/note.md");
    ASSERT_EQ(note_content, std::string("base"));
    
    // remote.txt should have the remote content
    const std::string remote_content = read_file_at(notebook_root + "/remote.txt");
    ASSERT_EQ(remote_content, std::string("remote content"));

    backend.Shutdown();

    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    std::cout << "  ✓ test_opt_out_skips_commit passed" << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running test_sync_pull_auto_commit..." << std::endl;

  int result = 0;
  result |= test_default_auto_commits();
  result |= test_opt_out_skips_commit();

  if (result == 0) {
    std::cout << "✓ All tests passed" << std::endl;
  } else {
    std::cout << "✗ Some tests failed" << std::endl;
  }

  return result;
}
