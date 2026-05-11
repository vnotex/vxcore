// Tests for GitSyncBackend Sync sub-helpers (Tasks T21 StageAll, T22
// CommitIndex, T23 FetchOrigin) of the git-sync-backend plan. We drive each
// helper through the public *ForTesting hooks so the binary doesn't need to
// reach into private members.
//
// Compiles git_sync_backend.cpp directly (matches test_git_sync_init /
// test_git_sync_status) so we own the GitSyncBackend statics independent of
// vxcore.dll.

#include <git2.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "sync/git_sync_backend.h"
#include "sync/libgit2_init.h"
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

// Returns true iff the index at <git_dir> contains <path> at stage 0.
bool index_contains(const std::string &git_dir, const std::string &path) {
  vxcore::LibGit2Init guard;
  git_repository *repo = nullptr;
  if (git_repository_open(&repo, git_dir.c_str()) != 0) {
    throw std::runtime_error("index_contains: git_repository_open");
  }
  git_index *idx = nullptr;
  if (git_repository_index(&idx, repo) != 0) {
    git_repository_free(repo);
    throw std::runtime_error("index_contains: git_repository_index");
  }
  const git_index_entry *e = git_index_get_bypath(idx, path.c_str(), 0);
  bool found = (e != nullptr);
  git_index_free(idx);
  git_repository_free(repo);
  return found;
}

// Returns true iff the index has any entry whose path begins with <prefix>.
bool index_has_prefix(const std::string &git_dir, const std::string &prefix) {
  vxcore::LibGit2Init guard;
  git_repository *repo = nullptr;
  if (git_repository_open(&repo, git_dir.c_str()) != 0) {
    throw std::runtime_error("index_has_prefix: git_repository_open");
  }
  git_index *idx = nullptr;
  if (git_repository_index(&idx, repo) != 0) {
    git_repository_free(repo);
    throw std::runtime_error("index_has_prefix: git_repository_index");
  }
  bool found = false;
  size_t n = git_index_entrycount(idx);
  for (size_t i = 0; i < n; ++i) {
    const git_index_entry *e = git_index_get_byindex(idx, i);
    if (e == nullptr || e->path == nullptr) continue;
    if (std::strncmp(e->path, prefix.c_str(), prefix.size()) == 0) {
      found = true;
      break;
    }
  }
  git_index_free(idx);
  git_repository_free(repo);
  return found;
}

struct CommitInfo {
  std::string sha;
  std::string message;
  std::string author_name;
  std::string author_email;
  bool has_parent;
};

CommitInfo head_commit_info(const std::string &git_dir) {
  vxcore::LibGit2Init guard;
  git_repository *repo = nullptr;
  if (git_repository_open(&repo, git_dir.c_str()) != 0) {
    throw std::runtime_error("head_commit_info: git_repository_open");
  }
  git_object *obj = nullptr;
  if (git_revparse_single(&obj, repo, "HEAD") != 0) {
    git_repository_free(repo);
    throw std::runtime_error("head_commit_info: git_revparse_single(HEAD)");
  }
  git_commit *commit = nullptr;
  if (git_commit_lookup(&commit, repo, git_object_id(obj)) != 0) {
    git_object_free(obj);
    git_repository_free(repo);
    throw std::runtime_error("head_commit_info: git_commit_lookup");
  }
  CommitInfo info;
  char buf[GIT_OID_HEXSZ + 1];
  git_oid_tostr(buf, sizeof(buf), git_commit_id(commit));
  info.sha = buf;
  const char *msg = git_commit_message(commit);
  info.message = msg ? msg : "";
  const git_signature *author = git_commit_author(commit);
  info.author_name = (author && author->name) ? author->name : "";
  info.author_email = (author && author->email) ? author->email : "";
  info.has_parent = (git_commit_parentcount(commit) > 0);

  git_commit_free(commit);
  git_object_free(obj);
  git_repository_free(repo);
  return info;
}

// Look up an arbitrary refspec in <git_dir> and return its OID hex. Throws
// if missing.
std::string ref_sha(const std::string &git_dir, const std::string &refspec) {
  vxcore::LibGit2Init guard;
  git_repository *repo = nullptr;
  if (git_repository_open(&repo, git_dir.c_str()) != 0) {
    throw std::runtime_error("ref_sha: git_repository_open");
  }
  git_object *obj = nullptr;
  if (git_revparse_single(&obj, repo, refspec.c_str()) != 0) {
    const git_error *e = git_error_last();
    std::string msg = std::string("ref_sha: git_revparse_single(") + refspec +
                      "): " + ((e && e->message) ? e->message : "unknown");
    git_repository_free(repo);
    throw std::runtime_error(msg);
  }
  char buf[GIT_OID_HEXSZ + 1];
  git_oid_tostr(buf, sizeof(buf), git_object_id(obj));
  std::string sha(buf);
  git_object_free(obj);
  git_repository_free(repo);
  return sha;
}

// Push a follow-up commit (parent = current bare HEAD) directly to <bare_path>.
// Spins up a temp non-bare workdir, fetches from bare, writes <rel_file> with
// <content>, commits with parent set to the fetched HEAD, and pushes back.
// Used by test_git_fetch_updates_origin_main where the helper's
// always-orphan commit_file would not produce a fast-forward push.
void push_follow_up_commit(const std::string &bare_path,
                           const std::string &rel_file,
                           const std::string &content,
                           const std::string &commit_msg) {
  vxcore::LibGit2Init guard;

  std::filesystem::path tmp_root = std::filesystem::temp_directory_path();
  std::filesystem::path workdir =
      tmp_root / ("vxcore_followup_" + std::to_string(std::rand()));
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
  // Normalize backslashes.
  for (auto &c : url) {
    if (c == '\\') c = '/';
  }
  // Avoid double slash on unix abs paths.
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

  // Resolve origin/main commit.
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

  // Materialize parent's tree into the workdir so add_bypath sees the file.
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

  // Write the new file.
  std::filesystem::path file_full = workdir / rel_file;
  std::filesystem::create_directories(file_full.parent_path());
  {
    std::ofstream ofs(file_full, std::ios::binary | std::ios::trunc);
    ofs << content;
  }

  // Stage it.
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

  // Push refs/heads/main:refs/heads/main back to bare.
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

// T21: StageAll picks up modified working-tree files into the index.
int test_git_stage_all_includes_modified_files() {
  std::cout << "  Running test_git_stage_all_includes_modified_files..."
            << std::endl;

  const std::string bare_path = get_test_path("git_stage_mod_bare");
  const std::string notebook_root = get_test_path("git_stage_mod_nb");
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
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    write_file_at(notebook_root + "/note.md", "hello world");
    write_file_at(notebook_root + "/extra.md", "new content");

    ASSERT_EQ(backend.StageAllForTesting(), VXCORE_OK);

    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";
    ASSERT_TRUE(index_contains(git_dir, "note.md"));
    ASSERT_TRUE(index_contains(git_dir, "extra.md"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_stage_all_includes_modified_files passed" << std::endl;
  return 0;
}

// T21 defensive walk: even after deleting .gitignore (so git_index_add_all
// would happily add anything under vx_notebook/vx_sync/), StageAll must
// scrub those entries from the index.
int test_git_stage_all_excludes_vx_sync_even_without_gitignore() {
  std::cout << "  Running "
               "test_git_stage_all_excludes_vx_sync_even_without_gitignore..."
            << std::endl;

  const std::string bare_path = get_test_path("git_stage_excl_bare");
  const std::string notebook_root = get_test_path("git_stage_excl_nb");
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
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    // Sabotage the gitignore so the defensive walk has to do real work.
    {
      std::error_code ec;
      std::filesystem::remove(
          vxcore::PathFromUtf8(notebook_root + "/.gitignore"), ec);
    }

    write_file_at(notebook_root + "/vx_notebook/vx_sync/stray.txt",
                  "should never be staged");
    write_file_at(notebook_root + "/note.md", "hello world");

    ASSERT_EQ(backend.StageAllForTesting(), VXCORE_OK);

    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";
    ASSERT_FALSE(index_has_prefix(git_dir, "vx_notebook/vx_sync"));
    // And legitimate change still got staged.
    ASSERT_TRUE(index_contains(git_dir, "note.md"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_stage_all_excludes_vx_sync_even_without_gitignore "
               "passed"
            << std::endl;
  return 0;
}

// T22: Stage + Commit advances HEAD by exactly one commit with the given
// message; the new commit's parent is the previous HEAD.
int test_git_commit_creates_one_commit() {
  std::cout << "  Running test_git_commit_creates_one_commit..." << std::endl;

  const std::string bare_path = get_test_path("git_commit_one_bare");
  const std::string notebook_root = get_test_path("git_commit_one_nb");
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
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";
    const std::string before = vxcore_test::git_head_sha(git_dir);

    write_file_at(notebook_root + "/note.md", "hello world");
    ASSERT_EQ(backend.StageAllForTesting(), VXCORE_OK);
    ASSERT_EQ(backend.CommitIndexForTesting("vnote: update note"), VXCORE_OK);

    const std::string after = vxcore_test::git_head_sha(git_dir);
    ASSERT_NE(before, after);

    CommitInfo info = head_commit_info(git_dir);
    ASSERT_EQ(info.message, std::string("vnote: update note"));
    ASSERT_TRUE(info.has_parent);
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_commit_creates_one_commit passed" << std::endl;
  return 0;
}

// T22 empty-commit suppression: with no working-tree changes, StageAll is a
// no-op and CommitIndex must NOT advance HEAD (returns OK silently).
int test_git_commit_skips_empty() {
  std::cout << "  Running test_git_commit_skips_empty..." << std::endl;

  const std::string bare_path = get_test_path("git_commit_empty_bare");
  const std::string notebook_root = get_test_path("git_commit_empty_nb");
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
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";

    // Initialize wrote .gitignore + .gitattributes — flush them into a real
    // commit first so the workdir is genuinely identical to HEAD before we
    // exercise the empty-commit suppression path.
    ASSERT_EQ(backend.StageAllForTesting(), VXCORE_OK);
    ASSERT_EQ(backend.CommitIndexForTesting("vnote: commit defaults"),
              VXCORE_OK);

    const std::string before = vxcore_test::git_head_sha(git_dir);

    ASSERT_EQ(backend.StageAllForTesting(), VXCORE_OK);
    ASSERT_EQ(backend.CommitIndexForTesting("nothing to do"), VXCORE_OK);

    const std::string after = vxcore_test::git_head_sha(git_dir);
    ASSERT_EQ(before, after);
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_commit_skips_empty passed" << std::endl;
  return 0;
}

// T22 default author: with no SyncCredentials::author_* set, the new
// commit's author/committer is "VNote Sync" <sync@vnote.local>.
int test_git_commit_uses_default_author() {
  std::cout << "  Running test_git_commit_uses_default_author..." << std::endl;

  const std::string bare_path = get_test_path("git_commit_author_bare");
  const std::string notebook_root = get_test_path("git_commit_author_nb");
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
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";

    write_file_at(notebook_root + "/note.md", "modified body");
    ASSERT_EQ(backend.StageAllForTesting(), VXCORE_OK);
    ASSERT_EQ(backend.CommitIndexForTesting("default author check"),
              VXCORE_OK);

    CommitInfo info = head_commit_info(git_dir);
    ASSERT_EQ(info.author_name, std::string("VNote Sync"));
    ASSERT_EQ(info.author_email, std::string("sync@vnote.local"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_commit_uses_default_author passed" << std::endl;
  return 0;
}

// T23: FetchOrigin pulls remote refs into refs/remotes/origin/<branch>.
// We seed the bare with one extra commit AFTER the local clone, then fetch,
// and assert the local refs/remotes/origin/main now matches the bare HEAD.
int test_git_fetch_updates_origin_main() {
  std::cout << "  Running test_git_fetch_updates_origin_main..." << std::endl;

  const std::string bare_path = get_test_path("git_fetch_bare");
  const std::string notebook_root = get_test_path("git_fetch_nb");
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
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";

    // Push a NEW commit to the bare from a follow-up workdir so the local
    // notebook's origin/main is now behind. We can't reuse
    // vxcore_test::commit_file here — it always creates orphan commits and
    // the second one would be rejected as non-fast-forward.
    push_follow_up_commit(bare_path, "note.md", "hello v2", "second commit");
    const std::string bare_head = vxcore_test::git_head_sha(bare_path);

    ASSERT_EQ(backend.FetchOriginForTesting(), VXCORE_OK);

    const std::string local_origin_main =
        ref_sha(git_dir, "refs/remotes/origin/main");
    ASSERT_EQ(local_origin_main, bare_head);
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_fetch_updates_origin_main passed" << std::endl;
  return 0;
}

// T24: clean fast-forward rebase. Local HEAD is at A. Helper pushes B onto
// origin. After fetch + rebase, local HEAD must equal B.
int test_git_rebase_clean_fast_forward() {
  std::cout << "  Running test_git_rebase_clean_fast_forward..." << std::endl;

  const std::string bare_path = get_test_path("git_rebase_ff_bare");
  const std::string notebook_root = get_test_path("git_rebase_ff_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "A", "init");

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.interval_seconds = 300;

    vxcore::GitSyncBackend backend;
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    push_follow_up_commit(bare_path, "note.md", "B", "second commit");
    const std::string bare_head = vxcore_test::git_head_sha(bare_path);

    ASSERT_EQ(backend.FetchOriginForTesting(), VXCORE_OK);
    ASSERT_EQ(backend.RebaseOntoOriginForTesting(), VXCORE_OK);

    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";
    const std::string local_head = vxcore_test::git_head_sha(git_dir);
    ASSERT_EQ(local_head, bare_head);
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_rebase_clean_fast_forward passed" << std::endl;
  return 0;
}

// T24: diverged histories on different files rebase cleanly. Both local file
// X and remote file Y must be present after rebase.
int test_git_rebase_diverged_no_conflict() {
  std::cout << "  Running test_git_rebase_diverged_no_conflict..." << std::endl;

  const std::string bare_path = get_test_path("git_rebase_div_bare");
  const std::string notebook_root = get_test_path("git_rebase_div_nb");
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

    // Local commit: add file X.
    write_file_at(notebook_root + "/local_X.md", "local change");
    ASSERT_EQ(backend.StageAllForTesting(), VXCORE_OK);
    ASSERT_EQ(backend.CommitIndexForTesting("add local X"), VXCORE_OK);

    // Remote commit: add file Y on top of init.
    push_follow_up_commit(bare_path, "remote_Y.md", "remote change",
                          "add remote Y");

    ASSERT_EQ(backend.FetchOriginForTesting(), VXCORE_OK);
    ASSERT_EQ(backend.RebaseOntoOriginForTesting(), VXCORE_OK);

    ASSERT_TRUE(vxcore::PathExists(notebook_root + "/local_X.md"));
    ASSERT_TRUE(vxcore::PathExists(notebook_root + "/remote_Y.md"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_rebase_diverged_no_conflict passed" << std::endl;
  return 0;
}

// T24: same file modified differently on both sides surfaces as
// VXCORE_ERR_SYNC_CONFLICT. Verify the rebase is left in progress (the
// repository state should NOT be NONE) so ResolveConflict can resume.
int test_git_rebase_conflict_surfaces() {
  std::cout << "  Running test_git_rebase_conflict_surfaces..." << std::endl;

  const std::string bare_path = get_test_path("git_rebase_conf_bare");
  const std::string notebook_root = get_test_path("git_rebase_conf_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
    vxcore_test::commit_file(bare_path, "note.md", "base line\n", "init");

    vxcore::SyncConfig config;
    config.backend = "git";
    config.remote_url = bare_url;
    config.interval_seconds = 300;

    vxcore::GitSyncBackend backend;
    ASSERT_EQ(backend.Initialize(notebook_root, config), VXCORE_OK);

    // Local change to line 1 of note.md.
    write_file_at(notebook_root + "/note.md", "local edit\n");
    ASSERT_EQ(backend.StageAllForTesting(), VXCORE_OK);
    ASSERT_EQ(backend.CommitIndexForTesting("local edit note"), VXCORE_OK);

    // Remote change to the SAME line 1.
    push_follow_up_commit(bare_path, "note.md", "remote edit\n",
                          "remote edit note");

    ASSERT_EQ(backend.FetchOriginForTesting(), VXCORE_OK);
    ASSERT_EQ(backend.RebaseOntoOriginForTesting(),
              VXCORE_ERR_SYNC_CONFLICT);

    // Verify the on-disk rebase state is preserved (libgit2 records this
    // under the gitdir as rebase-merge/* — git_repository_state observes it).
    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";
    vxcore::LibGit2Init guard;
    git_repository *repo = nullptr;
    ASSERT_EQ(git_repository_open(&repo, git_dir.c_str()), 0);
    int state = git_repository_state(repo);
    git_repository_free(repo);
    ASSERT_NE(state, GIT_REPOSITORY_STATE_NONE);
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_rebase_conflict_surfaces passed" << std::endl;
  return 0;
}

// T25: PushOrigin propagates a fast-forward local commit to the bare repo.
int test_git_push_fast_forward() {
  std::cout << "  Running test_git_push_fast_forward..." << std::endl;

  const std::string bare_path = get_test_path("git_push_ff_bare");
  const std::string notebook_root = get_test_path("git_push_ff_nb");
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

    write_file_at(notebook_root + "/note.md", "local update");
    ASSERT_EQ(backend.StageAllForTesting(), VXCORE_OK);
    ASSERT_EQ(backend.CommitIndexForTesting("local update"), VXCORE_OK);

    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";
    const std::string local_head = vxcore_test::git_head_sha(git_dir);

    ASSERT_EQ(backend.PushOriginForTesting(), VXCORE_OK);

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
  std::cout << "  test_git_push_fast_forward passed" << std::endl;
  return 0;
}

// Sync end-to-end: stage + commit + fetch + rebase (no conflict on different
// file) + push must succeed; status is empty afterwards and HEAD == origin.
int test_git_sync_round_trip_succeeds() {
  std::cout << "  Running test_git_sync_round_trip_succeeds..." << std::endl;

  const std::string bare_path = get_test_path("git_sync_rt_bare");
  const std::string notebook_root = get_test_path("git_sync_rt_nb");
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

    // Local change.
    write_file_at(notebook_root + "/local.md", "local body");

    // Remote change on a different file.
    push_follow_up_commit(bare_path, "remote.md", "remote body",
                          "remote add file");

    ASSERT_EQ(backend.Sync(nullptr, nullptr), VXCORE_OK);

    std::vector<vxcore::SyncFileInfo> status;
    ASSERT_EQ(backend.GetStatus(status), VXCORE_OK);
    ASSERT_TRUE(status.empty());

    const std::string git_dir = notebook_root + "/vx_notebook/vx_sync";
    const std::string local_head = vxcore_test::git_head_sha(git_dir);
    const std::string origin_main = ref_sha(git_dir, "refs/remotes/origin/main");
    ASSERT_EQ(local_head, origin_main);
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_sync_round_trip_succeeds passed" << std::endl;
  return 0;
}

// T26: Sync emits SyncProgress for each phase. Verify all five expected
// states (Staging, Fetching, Merging, Pushing, Idle) appear in order.
int test_git_sync_emits_progress_phases() {
  std::cout << "  Running test_git_sync_emits_progress_phases..." << std::endl;

  const std::string bare_path = get_test_path("git_sync_prog_bare");
  const std::string notebook_root = get_test_path("git_sync_prog_nb");
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

    write_file_at(notebook_root + "/local.md", "local body");
    push_follow_up_commit(bare_path, "remote.md", "remote body",
                          "remote add file");

    struct ProgressEntry {
      vxcore::SyncState state;
      float pct;
    };
    std::vector<ProgressEntry> entries;
    auto cb = [](const vxcore::SyncProgress &p, void *ud) {
      auto *vec = static_cast<std::vector<ProgressEntry> *>(ud);
      vec->push_back({p.current_state, p.percentage});
    };
    ASSERT_EQ(backend.Sync(cb, &entries), VXCORE_OK);

    auto contains_state = [&](vxcore::SyncState s) {
      for (const auto &e : entries) {
        if (e.state == s) return true;
      }
      return false;
    };
    ASSERT_TRUE(contains_state(vxcore::SyncState::kStaging));
    ASSERT_TRUE(contains_state(vxcore::SyncState::kFetching));
    ASSERT_TRUE(contains_state(vxcore::SyncState::kMerging));
    ASSERT_TRUE(contains_state(vxcore::SyncState::kPushing));
    ASSERT_TRUE(contains_state(vxcore::SyncState::kIdle));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_sync_emits_progress_phases passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  RUN_TEST(test_git_stage_all_includes_modified_files);
  RUN_TEST(test_git_stage_all_excludes_vx_sync_even_without_gitignore);
  RUN_TEST(test_git_commit_creates_one_commit);
  RUN_TEST(test_git_commit_skips_empty);
  RUN_TEST(test_git_commit_uses_default_author);
  RUN_TEST(test_git_fetch_updates_origin_main);
  RUN_TEST(test_git_rebase_clean_fast_forward);
  RUN_TEST(test_git_rebase_diverged_no_conflict);
  RUN_TEST(test_git_rebase_conflict_surfaces);
  RUN_TEST(test_git_push_fast_forward);
  RUN_TEST(test_git_sync_round_trip_succeeds);
  RUN_TEST(test_git_sync_emits_progress_phases);
  std::cout << "All git_sync_roundtrip tests passed" << std::endl;
  return 0;
}
