// T33 of git-sync-backend plan: GitSyncBackend::Push() and Pull() public
// methods (no C API). Verifies isolated push, isolated pull, and the
// CRITICAL Pull pre-commit semantic — uncommitted local edits MUST be
// auto-committed before fetch+rebase so they survive the pull.

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

// T33 Push: clone, modify a file, Push() — bare HEAD must equal local HEAD.
int test_git_push_only() {
  std::cout << "  Running test_git_push_only..." << std::endl;
  const std::string bare_path = get_test_path("git_pp_push_bare");
  const std::string notebook_root = get_test_path("git_pp_push_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "init", "init");

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.interval_seconds = 300;

    vxcore::GitSyncBackend backend;
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    write_file_at(notebook_root + "/note.md", "from push");
    ASSERT_EQ(backend.Push(nullptr, nullptr), VXCORE_OK);

    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";
    const std::string local_head = vxcore_test::git_head_sha(git_dir);
    const std::string bare_head = vxcore_test::git_head_sha(bare_path);
    ASSERT_EQ(local_head, bare_head);
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_push_only passed" << std::endl;
  return 0;
}

// T33 Pull: helper pushes a new file to bare; Pull() brings it down.
int test_git_pull_only() {
  std::cout << "  Running test_git_pull_only..." << std::endl;
  const std::string bare_path = get_test_path("git_pp_pull_bare");
  const std::string notebook_root = get_test_path("git_pp_pull_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "init", "init");

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.interval_seconds = 300;

    vxcore::GitSyncBackend backend;
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    push_follow_up_commit(bare_path, "remote_only.md", "remote body",
                          "remote add file");

    ASSERT_EQ(backend.Pull(nullptr, nullptr), VXCORE_OK);

    ASSERT_TRUE(vxcore::PathExists(notebook_root + "/remote_only.md"));
    const std::string body = read_file_at(notebook_root + "/remote_only.md");
    ASSERT_EQ(body, std::string("remote body"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_pull_only passed" << std::endl;
  return 0;
}

// T33 Pull pre-commit semantic: clone, modify note.md UNCOMMITTED, helper
// pushes a different file (other.md) to bare. Pull() must (1) auto-commit
// the local edit to note.md before fetching, (2) bring in other.md via
// rebase. After Pull, BOTH files reflect the expected state.
int test_git_pull_commits_local_first() {
  std::cout << "  Running test_git_pull_commits_local_first..." << std::endl;
  const std::string bare_path = get_test_path("git_pp_pullcommit_bare");
  const std::string notebook_root = get_test_path("git_pp_pullcommit_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "base", "init");

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.interval_seconds = 300;

    vxcore::GitSyncBackend backend;
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    // UNCOMMITTED local edit to note.md.
    write_file_at(notebook_root + "/note.md", "local edit not yet committed");

    // Remote pushes a different file.
    push_follow_up_commit(bare_path, "other.md", "remote other body",
                          "remote add other");

    ASSERT_EQ(backend.Pull(nullptr, nullptr), VXCORE_OK);

    // Verify both: local note.md change is committed AND other.md present.
    const std::string note_body = read_file_at(notebook_root + "/note.md");
    ASSERT_EQ(note_body, std::string("local edit not yet committed"));

    ASSERT_TRUE(vxcore::PathExists(notebook_root + "/other.md"));
    const std::string other_body = read_file_at(notebook_root + "/other.md");
    ASSERT_EQ(other_body, std::string("remote other body"));

    // Working tree must be clean post-Pull (both changes flushed to commits).
    std::vector<vxcore::SyncFileInfo> status;
    ASSERT_EQ(backend.GetStatus(status), VXCORE_OK);
    ASSERT_TRUE(status.empty());
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_pull_commits_local_first passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  RUN_TEST(test_git_push_only);
  RUN_TEST(test_git_pull_only);
  RUN_TEST(test_git_pull_commits_local_first);
  std::cout << "All git_sync_pushpull tests passed" << std::endl;
  return 0;
}
