// Tests for GitSyncBackend::Initialize re-open branch (Task T14 of the
// git-sync-backend plan). Each test case:
//   1. Builds a bare remote on disk via vxcore_test::create_bare_repo and
//      seeds it with one commit through vxcore_test::commit_file.
//   2. Manually creates a notebook root with a vx_notebook/vx_sync/ libgit2
//      repo using the spike T1 layout (separate gitdir under vx_sync, workdir
//      = notebook root, initial branch "main").
//   3. Drives GitSyncBackend::Initialize and asserts the documented behavior
//      (idempotent OK on URL match, INVALID_PARAM on URL mismatch).
//
// Other Initialize branches (T15 clone, T16 init+push, T17 reject) are
// covered by their own task cases in the same binary later — this file only
// owns the T14 cases.
//
// We compile git_sync_backend.cpp + libgit2_init.cpp directly into this
// binary (matching the test_git_sync_credentials pattern) so the test owns
// its own copy of the GitSyncBackend symbols and the libgit2 ref-count
// statics — the DLL boundary on Windows would otherwise duplicate the
// statics and confuse libgit2_init.

#include <git2.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "sync/git/git_defaults.h"
#include "sync/git/git_sync_backend.h"
#include "sync/git/libgit2_init.h"
#include "sync/sync_types.h"
#include "test_git_sync_helpers.h"
#include "test_internals/mock_credential_provider.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore_types.h"

namespace {

// Manually construct a vx_notebook/vx_sync/ repo at <notebook_root> bound to
// <origin_url>, mirroring the layout GitSyncBackend::Initialize will re-open.
// Keeps the dependency story narrow — no GitSyncBackend init code involved.
void seed_notebook_with_existing_repo(const std::string &notebook_root,
                                      const std::string &origin_url) {
  vxcore::LibGit2Init guard;

  std::filesystem::create_directories(notebook_root);

  const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";

  git_repository_init_options opts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
  if (git_repository_init_options_init(&opts,
                                       GIT_REPOSITORY_INIT_OPTIONS_VERSION) < 0) {
    throw std::runtime_error("init_options_init failed");
  }
  // NO_DOTGIT_DIR: keep the gitdir literally at <notebook_root>/vx_notebook/vx_sync
  // instead of letting libgit2 nest a ".git" subfolder, since GitSyncBackend
  // looks for HEAD directly under that path.
  opts.flags = GIT_REPOSITORY_INIT_MKDIR | GIT_REPOSITORY_INIT_MKPATH |
               GIT_REPOSITORY_INIT_NO_REINIT |
               GIT_REPOSITORY_INIT_NO_DOTGIT_DIR;
  opts.workdir_path = notebook_root.c_str();
  opts.initial_head = "main";

  git_repository *repo = nullptr;
  if (git_repository_init_ext(&repo, git_dir.c_str(), &opts) < 0) {
    const git_error *e = git_error_last();
    throw std::runtime_error(std::string("git_repository_init_ext: ") +
                             ((e && e->message) ? e->message : "unknown"));
  }

  git_remote *remote = nullptr;
  int rc = git_remote_create(&remote, repo, "origin", origin_url.c_str());
  if (rc != 0) {
    const git_error *e = git_error_last();
    git_repository_free(repo);
    throw std::runtime_error(std::string("git_remote_create: ") +
                             ((e && e->message) ? e->message : "unknown"));
  }

  git_remote_free(remote);
  git_repository_free(repo);
}

int test_git_init_reopens_existing_repo() {
  std::cout << "  Running test_git_init_reopens_existing_repo..." << std::endl;

  const std::string bare_path = get_test_path("git_init_reopen_bare");
  const std::string notebook_root = get_test_path("git_init_reopen_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "hello", "init");

    seed_notebook_with_existing_repo(notebook_root, bare_url);

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.interval_seconds = 300;

    vxcore::GitSyncBackend backend;
    VxCoreError rc = backend.Initialize(notebook_root, config);
    ASSERT_EQ(rc, VXCORE_OK);

    // Idempotent re-call returns OK without re-validating.
    rc = backend.Initialize(notebook_root, config);
    ASSERT_EQ(rc, VXCORE_OK);
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_init_reopens_existing_repo passed" << std::endl;
  return 0;
}

int test_git_init_rejects_url_mismatch() {
  std::cout << "  Running test_git_init_rejects_url_mismatch..." << std::endl;

  const std::string bare_path_a = get_test_path("git_init_mismatch_bare_a");
  const std::string bare_path_b = get_test_path("git_init_mismatch_bare_b");
  const std::string notebook_root = get_test_path("git_init_mismatch_nb");
  cleanup_test_dir(bare_path_a);
  cleanup_test_dir(bare_path_b);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url_a = vxcore_test::create_bare_repo(bare_path_a);
    const std::string bare_url_b = vxcore_test::create_bare_repo(bare_path_b);
    vxcore_test::commit_file(bare_path_a, "note.md", "hello", "init");

    // Existing repo points at bare A.
    seed_notebook_with_existing_repo(notebook_root, bare_url_a);

    // Config points at bare B — Initialize must reject.
    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url_b;
    config.interval_seconds = 300;

    vxcore::GitSyncBackend backend;
    VxCoreError rc = backend.Initialize(notebook_root, config);
    ASSERT_EQ(rc, VXCORE_ERR_INVALID_PARAM);
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path_a);
    cleanup_test_dir(bare_path_b);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path_a);
  cleanup_test_dir(bare_path_b);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_init_rejects_url_mismatch passed" << std::endl;
  return 0;
}

// T17: vx_sync does not exist, the notebook root has user files, and the
// remote already has refs — Initialize must refuse with INVALID_PARAM.
int test_git_init_rejects_both_non_empty() {
  std::cout << "  Running test_git_init_rejects_both_non_empty..." << std::endl;

  const std::string bare_path = get_test_path("git_init_bothfull_bare");
  const std::string notebook_root = get_test_path("git_init_bothfull_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "remote.md", "remote content", "init");

    // Notebook root has a stray user file but NO vx_notebook/vx_sync/.
    std::filesystem::create_directories(vxcore::PathFromUtf8(notebook_root));
    {
      std::ofstream ofs(vxcore::PathFromUtf8(notebook_root + "/notes.md"),
                        std::ios::binary | std::ios::trunc);
      ofs << "local user content";
    }

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.interval_seconds = 300;

    vxcore::GitSyncBackend backend;
    VxCoreError rc = backend.Initialize(notebook_root, config);
    ASSERT_EQ(rc, VXCORE_ERR_INVALID_PARAM);
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_init_rejects_both_non_empty passed" << std::endl;
  return 0;
}

// T17: vx_notebook/vx_sync/ exists as a directory but contains no HEAD file —
// Initialize must refuse and leave the directory contents intact (no auto
// delete of user data).
int test_git_init_rejects_corrupt_repo() {
  std::cout << "  Running test_git_init_rejects_corrupt_repo..." << std::endl;

  const std::string notebook_root = get_test_path("git_init_corrupt_nb");
  cleanup_test_dir(notebook_root);

  try {
    const std::string vx_sync = notebook_root + "/vx_notebook/vx_sync";
    std::filesystem::create_directories(vxcore::PathFromUtf8(vx_sync));
    const std::string stray = vx_sync + "/stray.txt";
    {
      std::ofstream ofs(vxcore::PathFromUtf8(stray), std::ios::binary | std::ios::trunc);
      ofs << "garbage";
    }

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = "file:///tmp/does-not-matter.git";
    config.interval_seconds = 300;

    vxcore::GitSyncBackend backend;
    VxCoreError rc = backend.Initialize(notebook_root, config);
    ASSERT_NE(rc, VXCORE_OK);

    // Garbage left intact (no auto-delete of user data).
    ASSERT_TRUE(vxcore::IsRegularFile(stray));
    ASSERT_TRUE(vxcore::IsDirectory(vx_sync));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_init_rejects_corrupt_repo passed" << std::endl;
  return 0;
}

// Read a config string value from a libgit2 repo at <repo_path>. Throws if
// missing. Used by the T18 assertion helpers.
namespace {

std::string read_config_string(const std::string &repo_path, const char *key) {
  git_repository *repo = nullptr;
  if (git_repository_open(&repo, repo_path.c_str()) != 0) {
    throw std::runtime_error("git_repository_open failed");
  }
  git_config *cfg = nullptr;
  if (git_repository_config(&cfg, repo) != 0) {
    git_repository_free(repo);
    throw std::runtime_error("git_repository_config failed");
  }
  git_buf buf = GIT_BUF_INIT;
  int rc = git_config_get_string_buf(&buf, cfg, key);
  if (rc != 0) {
    git_config_free(cfg);
    git_repository_free(repo);
    throw std::runtime_error(std::string("git_config_get_string_buf(") + key + ") failed");
  }
  std::string out(buf.ptr ? buf.ptr : "");
  git_buf_dispose(&buf);
  git_config_free(cfg);
  git_repository_free(repo);
  return out;
}

int read_config_bool(const std::string &repo_path, const char *key) {
  git_repository *repo = nullptr;
  if (git_repository_open(&repo, repo_path.c_str()) != 0) {
    throw std::runtime_error("git_repository_open failed");
  }
  git_config *cfg = nullptr;
  if (git_repository_config(&cfg, repo) != 0) {
    git_repository_free(repo);
    throw std::runtime_error("git_repository_config failed");
  }
  int value = 0;
  int rc = git_config_get_bool(&value, cfg, key);
  git_config_free(cfg);
  git_repository_free(repo);
  if (rc != 0) {
    throw std::runtime_error(std::string("git_config_get_bool(") + key + ") failed");
  }
  return value;
}

}  // namespace

// T18: re-open path applies baseline git config (filemode/autocrlf/gpgsign +
// default author).
int test_git_init_applies_config() {
  std::cout << "  Running test_git_init_applies_config..." << std::endl;

  const std::string bare_path = get_test_path("git_init_cfg_bare");
  const std::string notebook_root = get_test_path("git_init_cfg_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "hello", "init");

    seed_notebook_with_existing_repo(notebook_root, bare_url);

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.interval_seconds = 300;

    {
      vxcore::GitSyncBackend backend;
      VxCoreError rc = backend.Initialize(notebook_root, config);
      ASSERT_EQ(rc, VXCORE_OK);
    }  // Shutdown frees the repo so we can reopen below.

    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";
    vxcore::LibGit2Init guard;
    ASSERT_EQ(read_config_bool(git_dir, "core.filemode"), 0);
    ASSERT_EQ(read_config_string(git_dir, "core.autocrlf"), std::string("false"));
    ASSERT_EQ(read_config_bool(git_dir, "commit.gpgsign"), 0);
    ASSERT_EQ(read_config_string(git_dir, "user.name"), std::string("VNote Sync"));
    ASSERT_EQ(read_config_string(git_dir, "user.email"), std::string("sync@vnote.local"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_init_applies_config passed" << std::endl;
  return 0;
}

// T18: SyncCredentials::author_name/author_email override the hardcoded
// defaults when set BEFORE Initialize.
int test_git_init_uses_credential_author_when_set() {
  std::cout << "  Running test_git_init_uses_credential_author_when_set..." << std::endl;

  const std::string bare_path = get_test_path("git_init_author_bare");
  const std::string notebook_root = get_test_path("git_init_author_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "hello", "init");

    seed_notebook_with_existing_repo(notebook_root, bare_url);

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.interval_seconds = 300;

    {
      vxcore::GitSyncBackend backend;
      auto provider = std::make_shared<vxcore::MockCredentialProvider>();
      vxcore::SyncCredentials creds;
      creds.author_name = "Alice";
      creds.author_email = "alice@a.com";
      provider->SetCredentials(creds);
      backend.ReplaceCredsProvider(provider);
      VxCoreError rc = backend.Initialize(notebook_root, config);
      ASSERT_EQ(rc, VXCORE_OK);
    }

    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";
    vxcore::LibGit2Init guard;
    ASSERT_EQ(read_config_string(git_dir, "user.name"), std::string("Alice"));
    ASSERT_EQ(read_config_string(git_dir, "user.email"), std::string("alice@a.com"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_init_uses_credential_author_when_set passed" << std::endl;
  return 0;
}

// T15: clone-into-empty branch. With no vx_sync, no user files, and a remote
// that has commits, Initialize must clone into the notebook root using the
// init+remote+fetch+checkout pattern (NOT git_clone — that hangs on Windows
// for file:// URLs).
int test_git_init_clone_into_empty_notebook() {
  std::cout << "  Running test_git_init_clone_into_empty_notebook..." << std::endl;

  const std::string bare_path = get_test_path("git_init_clone_bare");
  const std::string notebook_root = get_test_path("git_init_clone_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "hello", "init");

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.interval_seconds = 300;

    vxcore::GitSyncBackend backend;
    VxCoreError rc = backend.Initialize(notebook_root, config);
    ASSERT_EQ(rc, VXCORE_OK);

    // The remote file checked out at the workdir (notebook root).
    const std::string note_path = notebook_root + "/note.md";
    ASSERT_TRUE(vxcore::IsRegularFile(note_path));
    {
      std::ifstream ifs(vxcore::PathFromUtf8(note_path), std::ios::binary);
      std::string content((std::istreambuf_iterator<char>(ifs)),
                          std::istreambuf_iterator<char>());
      ASSERT_EQ(content, std::string("hello"));
    }

    // gitdir is at vx_notebook/vx_sync/, NOT at <root>/.git.
    ASSERT_FALSE(vxcore::PathExists(notebook_root + "/.git"));
    ASSERT_TRUE(vxcore::IsRegularFile(notebook_root + "/vx_notebook/vx_sync/HEAD"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_init_clone_into_empty_notebook passed" << std::endl;
  return 0;
}

// T15: SetCredentials before Initialize must not break the clone path even
// when the remote (file://) doesn't actually authenticate. The credential
// callback wires through but libgit2's local transport never invokes it.
int test_git_init_clone_with_pat_uses_3arg_overload() {
  std::cout << "  Running test_git_init_clone_with_pat_uses_3arg_overload..."
            << std::endl;

  const std::string bare_path = get_test_path("git_init_clonepat_bare");
  const std::string notebook_root = get_test_path("git_init_clonepat_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "hello", "init");

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.interval_seconds = 300;

    vxcore::GitSyncBackend backend;
    auto provider = std::make_shared<vxcore::MockCredentialProvider>();
    vxcore::SyncCredentials creds;
    creds.personal_access_token = "dummy";
    provider->SetCredentials(creds);
    backend.ReplaceCredsProvider(provider);

    VxCoreError rc = backend.Initialize(notebook_root, config);
    ASSERT_EQ(rc, VXCORE_OK);
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_init_clone_with_pat_uses_3arg_overload passed"
            << std::endl;
  return 0;
}

// T15: explicit assertion that the clone branch uses the separate gitdir
// layout (vx_notebook/vx_sync/) and NEVER creates a top-level .git directory.
int test_git_init_clone_uses_separate_gitdir() {
  std::cout << "  Running test_git_init_clone_uses_separate_gitdir..."
            << std::endl;

  const std::string bare_path = get_test_path("git_init_clonegit_bare");
  const std::string notebook_root = get_test_path("git_init_clonegit_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "hello", "init");

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.interval_seconds = 300;

    vxcore::GitSyncBackend backend;
    VxCoreError rc = backend.Initialize(notebook_root, config);
    ASSERT_EQ(rc, VXCORE_OK);

    ASSERT_FALSE(vxcore::PathExists(notebook_root + "/.git"));
    ASSERT_TRUE(vxcore::IsRegularFile(notebook_root + "/vx_notebook/vx_sync/HEAD"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_init_clone_uses_separate_gitdir passed" << std::endl;
  return 0;
}

// T16: init+push branch. With no vx_sync, user files in the notebook root,
// and an EMPTY remote (no refs yet), Initialize must init a local repo,
// commit the user files, and push refs/heads/main so the remote becomes the
// source of truth. Bare's HEAD must end up at the same SHA as the local
// vx_sync HEAD, and the pushed tree must contain notes.md.
int test_git_init_push_existing_notebook_to_empty_remote() {
  std::cout << "  Running test_git_init_push_existing_notebook_to_empty_remote..."
            << std::endl;

  const std::string bare_path = get_test_path("git_init_push_bare");
  const std::string notebook_root = get_test_path("git_init_push_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    // Bare remote with NO commits — RemoteHasRefs() must return false so we
    // exercise the T16 branch (not T17 reject).
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);

    // Notebook root with a single user file; no vx_notebook/vx_sync/.
    std::filesystem::create_directories(vxcore::PathFromUtf8(notebook_root));
    {
      std::ofstream ofs(vxcore::PathFromUtf8(notebook_root + "/notes.md"),
                        std::ios::binary | std::ios::trunc);
      ofs << "local user content";
    }

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.interval_seconds = 300;

    {
      vxcore::GitSyncBackend backend;
      VxCoreError rc = backend.Initialize(notebook_root, config);
      ASSERT_EQ(rc, VXCORE_OK);
    }  // Shutdown frees repo so we can reopen below.

    const std::string local_gitdir = notebook_root + "/vx_notebook/vx_sync";

    // gitdir layout: separate gitdir at vx_notebook/vx_sync/, no top-level .git.
    ASSERT_FALSE(vxcore::PathExists(notebook_root + "/.git"));
    ASSERT_TRUE(vxcore::IsRegularFile(local_gitdir + "/HEAD"));

    // Bare and local must point at the same commit after the push.
    const std::string local_sha = vxcore_test::git_head_sha(local_gitdir);
    const std::string bare_sha = vxcore_test::git_head_sha(bare_path);
    ASSERT_EQ(local_sha, bare_sha);

    // The pushed commit's tree must contain notes.md.
    vxcore::LibGit2Init guard;
    git_repository *repo = nullptr;
    ASSERT_EQ(git_repository_open(&repo, bare_path.c_str()), 0);

    git_object *head_obj = nullptr;
    int rc = git_revparse_single(&head_obj, repo, "HEAD");
    if (rc != 0) {
      git_repository_free(repo);
      throw std::runtime_error("git_revparse_single(HEAD) failed");
    }
    git_commit *commit = nullptr;
    rc = git_commit_lookup(&commit, repo, git_object_id(head_obj));
    git_object_free(head_obj);
    if (rc != 0) {
      git_repository_free(repo);
      throw std::runtime_error("git_commit_lookup failed");
    }
    git_tree *tree = nullptr;
    rc = git_commit_tree(&tree, commit);
    git_commit_free(commit);
    if (rc != 0) {
      git_repository_free(repo);
      throw std::runtime_error("git_commit_tree failed");
    }

    const git_tree_entry *entry = git_tree_entry_byname(tree, "notes.md");
    ASSERT_NOT_NULL(entry);

    git_tree_free(tree);
    git_repository_free(repo);
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_init_push_existing_notebook_to_empty_remote passed"
            << std::endl;
  return 0;
}

// T19: removed in Wave 4.5 (F1.3 of sync-backend-phase4) — the standalone
// Shutdown() virtual is gone; teardown happens implicitly via the destructor.
// The destructor-only path is exercised by every other test that constructs a
// GitSyncBackend, so no replacement test is needed here.

// Default .gitignore + .gitattributes writer tests. Migrated from
// test_sync_manager_dispatch.cpp during T14 because they now exercise
// vxcore::WriteIfMissing + vxcore::BuildGitignoreContent directly (the
// GitSyncBackend::WriteDefaultIgnoreAndAttributesForTesting shim was
// removed). This binary already direct-compiles git_defaults.cpp so the
// helpers are reachable without any DLL-export gymnastics.
int test_git_sync_writes_default_gitignore_attributes_when_missing() {
  std::cout << "  Running test_git_sync_writes_default_gitignore_attributes_when_missing..."
            << std::endl;
  std::string dir = get_test_path("test_git_sync_defaults_missing");
  cleanup_test_dir(dir);
  std::filesystem::create_directories(vxcore::PathFromUtf8(dir));

  vxcore::SyncConfig config;
  config.exclude_paths = {"*.bak", "scratch/"};

  int written = 0;
  if (vxcore::WriteIfMissing(dir + "/.gitignore", vxcore::BuildGitignoreContent(config))) {
    ++written;
  }
  if (vxcore::WriteIfMissing(dir + "/.gitattributes", vxcore::kDefaultGitattributes)) {
    ++written;
  }
  ASSERT_EQ(written, 2);

  std::string gi_path = dir + "/.gitignore";
  std::string ga_path = dir + "/.gitattributes";
  ASSERT_TRUE(vxcore::PathExists(gi_path));
  ASSERT_TRUE(vxcore::PathExists(ga_path));

  {
    std::ifstream ifs(vxcore::PathFromUtf8(gi_path), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    ASSERT_TRUE(content.find("vx_notebook/vx_sync/") != std::string::npos);
    ASSERT_TRUE(content.find("*.bak") != std::string::npos);
    ASSERT_TRUE(content.find("scratch/") != std::string::npos);
  }
  {
    std::ifstream ifs(vxcore::PathFromUtf8(ga_path), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    ASSERT_TRUE(content.find("text=auto eol=lf") != std::string::npos);
    ASSERT_TRUE(content.find("*.png binary") != std::string::npos);
  }

  cleanup_test_dir(dir);
  std::cout
      << "  \xE2\x9C\x93 test_git_sync_writes_default_gitignore_attributes_when_missing passed"
      << std::endl;
  return 0;
}

int test_git_sync_preserves_existing_gitignore() {
  std::cout << "  Running test_git_sync_preserves_existing_gitignore..." << std::endl;
  std::string dir = get_test_path("test_git_sync_defaults_preserve");
  cleanup_test_dir(dir);
  std::filesystem::create_directories(vxcore::PathFromUtf8(dir));

  std::string gi_path = dir + "/.gitignore";
  std::string ga_path = dir + "/.gitattributes";
  const std::string user_content = "# user customization\n*.user\n";
  {
    std::ofstream ofs(vxcore::PathFromUtf8(gi_path), std::ios::binary | std::ios::trunc);
    ofs.write(user_content.data(), static_cast<std::streamsize>(user_content.size()));
  }

  vxcore::SyncConfig config;
  int written = 0;
  if (vxcore::WriteIfMissing(gi_path, vxcore::BuildGitignoreContent(config))) {
    ++written;
  }
  if (vxcore::WriteIfMissing(ga_path, vxcore::kDefaultGitattributes)) {
    ++written;
  }
  ASSERT_EQ(written, 1);

  {
    std::ifstream ifs(vxcore::PathFromUtf8(gi_path), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    ASSERT_TRUE(content == user_content);
  }
  ASSERT_TRUE(vxcore::PathExists(ga_path));
  {
    std::ifstream ifs(vxcore::PathFromUtf8(ga_path), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    ASSERT_TRUE(content.find("text=auto eol=lf") != std::string::npos);
  }

  cleanup_test_dir(dir);
  std::cout << "  \xE2\x9C\x93 test_git_sync_preserves_existing_gitignore passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  RUN_TEST(test_git_init_reopens_existing_repo);
  RUN_TEST(test_git_init_rejects_url_mismatch);
  RUN_TEST(test_git_init_rejects_both_non_empty);
  RUN_TEST(test_git_init_rejects_corrupt_repo);
  RUN_TEST(test_git_init_applies_config);
  RUN_TEST(test_git_init_uses_credential_author_when_set);
  RUN_TEST(test_git_init_clone_into_empty_notebook);
  RUN_TEST(test_git_init_clone_with_pat_uses_3arg_overload);
  RUN_TEST(test_git_init_clone_uses_separate_gitdir);
  RUN_TEST(test_git_init_push_existing_notebook_to_empty_remote);
  RUN_TEST(test_git_sync_writes_default_gitignore_attributes_when_missing);
  RUN_TEST(test_git_sync_preserves_existing_gitignore);
  std::cout << "All git_sync_init tests passed" << std::endl;
  return 0;
}
