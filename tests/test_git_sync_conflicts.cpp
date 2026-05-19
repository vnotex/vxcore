// T29-T32 of git-sync-backend plan: GetConflicts + ResolveConflict variants.
// Setup pattern mirrors test_git_rebase_conflict_surfaces in
// test_git_sync_roundtrip.cpp: clone, modify same line locally + push
// different content via push_follow_up_commit, then FetchOriginForTesting +
// RebaseOntoOriginForTesting -> SYNC_CONFLICT.

#include <git2.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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

// Mirror push_follow_up_commit from test_git_sync_roundtrip.cpp so we can
// drive the bare repo onto a divergent line and force a real conflict.
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

// Drive a backend into a SYNC_CONFLICT state on note.md.
// On return, GetConflicts will surface "note.md" as the unresolved file.
// |local_content| and |remote_content| are written to the same file with the
// same starting base content so the rebase has a real text conflict.
void setup_conflicted_backend(vxcore::GitSyncBackend &backend,
                              const std::string &bare_path,
                              const std::string &notebook_root,
                              const std::string &local_content,
                              const std::string &remote_content) {
  const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
  vxcore_test::commit_file(bare_path, "note.md", "base line\n", "init");

  vxcore::SyncConfig config;
  config.backend = "git";
  config.remote_url = bare_url;
  config.interval_seconds = 300;

  if (backend.Initialize(notebook_root, config) != VXCORE_OK) {
    throw std::runtime_error("setup: Initialize failed");
  }

  write_file_at(notebook_root + "/note.md", local_content);
  if (backend.MakePipeline()->StageAll() != VXCORE_OK) {
    throw std::runtime_error("setup: StageAll failed");
  }
  if (backend.MakePipeline()->CommitIndex("local edit") != VXCORE_OK) {
    throw std::runtime_error("setup: CommitIndex failed");
  }

  push_follow_up_commit(bare_path, "note.md", remote_content, "remote edit");

  if (backend.MakePipeline()->FetchOrigin() != VXCORE_OK) {
    throw std::runtime_error("setup: FetchOrigin failed");
  }
  if (backend.MakePipeline()->RebaseOntoOrigin() != VXCORE_ERR_SYNC_CONFLICT) {
    throw std::runtime_error("setup: rebase did not surface conflict");
  }
}

// T29: GetConflicts after rebase enumerates the unresolved file with
// reasonable metadata.
int test_git_get_conflicts_after_rebase() {
  std::cout << "  Running test_git_get_conflicts_after_rebase..." << std::endl;
  const std::string bare_path = get_test_path("git_conf_get_bare");
  const std::string notebook_root = get_test_path("git_conf_get_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    vxcore::GitSyncBackend backend;
    setup_conflicted_backend(backend, bare_path, notebook_root,
                             "local edit\n", "remote edit\n");

    std::vector<vxcore::SyncConflictInfo> conflicts;
    ASSERT_EQ(backend.GetConflicts(conflicts), VXCORE_OK);
    ASSERT_EQ(conflicts.size(), static_cast<size_t>(1));
    ASSERT_EQ(conflicts[0].path, std::string("note.md"));
    ASSERT_FALSE(conflicts[0].is_binary);
    ASSERT_TRUE(conflicts[0].remote_modified_utc > 0);
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_get_conflicts_after_rebase passed" << std::endl;
  return 0;
}

// T30: kKeepBoth writes the LOCAL content into a file.sync-conflict-<ts>.md
// sibling, overwrites the original with REMOTE content, and clears the
// conflict from the index. After ContinueRebaseAfterResolution the rebase
// finishes (no further conflicts) so HEAD has advanced.
int test_git_resolve_keep_both_creates_conflict_file() {
  std::cout << "  Running test_git_resolve_keep_both_creates_conflict_file..."
            << std::endl;
  const std::string bare_path = get_test_path("git_conf_kb_bare");
  const std::string notebook_root = get_test_path("git_conf_kb_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    vxcore::GitSyncBackend backend;
    setup_conflicted_backend(backend, bare_path, notebook_root,
                             "LOCAL\n", "REMOTE\n");

    ASSERT_EQ(backend.ResolveConflict("note.md",
                                      vxcore::SyncConflictResolution::kKeepBoth),
              VXCORE_OK);

    // Original file now holds REMOTE content.
    const std::string note_body = read_file_at(notebook_root + "/note.md");
    ASSERT_EQ(note_body, std::string("REMOTE\n"));

    // A sibling note.sync-conflict-<ts>.md exists with LOCAL content.
    bool found_conflict_file = false;
    for (auto &entry : std::filesystem::directory_iterator(
             vxcore::PathFromUtf8(notebook_root))) {
      const std::string name =
          entry.path().filename().u8string();
      if (name.rfind("note.sync-conflict-", 0) == 0 &&
          name.size() > std::strlen("note.sync-conflict-")) {
        const std::string body =
            read_file_at(notebook_root + "/" + name);
        if (body == "LOCAL\n") {
          found_conflict_file = true;
        }
        break;
      }
    }
    ASSERT_TRUE(found_conflict_file);

    // Conflicts list should now be empty.
    std::vector<vxcore::SyncConflictInfo> remaining;
    ASSERT_EQ(backend.GetConflicts(remaining), VXCORE_OK);
    ASSERT_TRUE(remaining.empty());
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_resolve_keep_both_creates_conflict_file passed"
            << std::endl;
  return 0;
}

// T31: kKeepLocal stages the workdir copy and clears the conflict. Original
// file is unchanged on disk.
int test_git_resolve_keep_local() {
  std::cout << "  Running test_git_resolve_keep_local..." << std::endl;
  const std::string bare_path = get_test_path("git_conf_kl_bare");
  const std::string notebook_root = get_test_path("git_conf_kl_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    vxcore::GitSyncBackend backend;
    setup_conflicted_backend(backend, bare_path, notebook_root,
                             "LOCAL\n", "REMOTE\n");

    ASSERT_EQ(backend.ResolveConflict(
                  "note.md", vxcore::SyncConflictResolution::kKeepLocal),
              VXCORE_OK);

    const std::string note_body = read_file_at(notebook_root + "/note.md");
    ASSERT_EQ(note_body, std::string("LOCAL\n"));

    std::vector<vxcore::SyncConflictInfo> remaining;
    ASSERT_EQ(backend.GetConflicts(remaining), VXCORE_OK);
    ASSERT_TRUE(remaining.empty());
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_resolve_keep_local passed" << std::endl;
  return 0;
}

// T32: kKeepRemote overwrites workdir with THEIR content and clears the
// conflict.
int test_git_resolve_keep_remote() {
  std::cout << "  Running test_git_resolve_keep_remote..." << std::endl;
  const std::string bare_path = get_test_path("git_conf_kr_bare");
  const std::string notebook_root = get_test_path("git_conf_kr_nb");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);

  try {
    vxcore::GitSyncBackend backend;
    setup_conflicted_backend(backend, bare_path, notebook_root,
                             "LOCAL\n", "REMOTE\n");

    ASSERT_EQ(backend.ResolveConflict(
                  "note.md", vxcore::SyncConflictResolution::kKeepRemote),
              VXCORE_OK);

    const std::string note_body = read_file_at(notebook_root + "/note.md");
    ASSERT_EQ(note_body, std::string("REMOTE\n"));

    std::vector<vxcore::SyncConflictInfo> remaining;
    ASSERT_EQ(backend.GetConflicts(remaining), VXCORE_OK);
    ASSERT_TRUE(remaining.empty());
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    cleanup_test_dir(bare_path);
    cleanup_test_dir(notebook_root);
    return 1;
  }

  cleanup_test_dir(bare_path);
  cleanup_test_dir(notebook_root);
  std::cout << "  test_git_resolve_keep_remote passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  RUN_TEST(test_git_get_conflicts_after_rebase);
  RUN_TEST(test_git_resolve_keep_both_creates_conflict_file);
  RUN_TEST(test_git_resolve_keep_local);
  RUN_TEST(test_git_resolve_keep_remote);
  std::cout << "All git_sync_conflicts tests passed" << std::endl;
  return 0;
}
