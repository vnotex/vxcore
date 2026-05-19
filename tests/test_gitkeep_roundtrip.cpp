// T4 of the gitkeep-empty-folders plan. RED-state test for the end-to-end
// promise of the feature: an empty user-visible folder created in a sync-
// enabled notebook must survive a full Sync() round-trip (stage + commit +
// push) AND a subsequent clone of the remote into a fresh working tree.
//
// This is the only test in the wave that actually inspects the remote-side
// truth, by cloning the bare repo into a separate temp directory and asserting
// the .gitkeep distribution there. Because EnsureGitkeepFiles is not yet
// implemented, the empty MyEmpty/ folder never gets a marker, never enters
// the commit, never appears on the bare remote, and therefore never appears
// in the clone — so the existence assertions fail and the test exits non-zero.
//
// Mirrors the fixture pattern from test_gitkeep_basic.cpp and the libgit2
// clone-replacement pattern (init + remote + fetch + checkout) used inside
// test_git_sync_init.cpp / git_sync_backend.cpp, since git_clone() over
// file:// URLs hangs on Windows.

#include <git2.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
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

// vxcore_set_test_mode lives in vxcore.dll which is NOT linked into this
// test binary (we only compile git_sync_backend.cpp + utils.cpp + the
// git_sync_test_helpers static lib, matching test_gitkeep_basic.cpp). The
// production ConfigManager test-mode flag is moot here because get_test_path()
// already places everything under %TEMP%. Define a local no-op so the mandated
// vxcore_set_test_mode(1) call in main() links cleanly. Anonymous namespace
// prevents collision with the real C-linkage symbol if anyone ever links
// vxcore.dll into a sibling test.
void vxcore_set_test_mode(int /*enabled*/) {}

// Fixture: a sync-enabled notebook backed by a fresh local bare repo.
struct Fixture {
  std::string bare_path;
  std::string notebook_root;
  std::string clone_dir;
  vxcore::GitSyncBackend backend;
};

VxCoreError setup_synced_notebook(Fixture &f, const std::string &test_tag) {
  f.bare_path = get_test_path(std::string("gitkeep_rt_") + test_tag + "_bare");
  f.notebook_root =
      get_test_path(std::string("gitkeep_rt_") + test_tag + "_nb");
  f.clone_dir = get_test_path(std::string("gitkeep_rt_") + test_tag + "_clone");
  cleanup_test_dir(f.bare_path);
  cleanup_test_dir(f.notebook_root);
  cleanup_test_dir(f.clone_dir);

  const std::string bare_url = vxcore_test::create_bare_repo(f.bare_path);
  vxcore_test::commit_file(f.bare_path, "seed.md", "seed body", "init");

  vxcore::SyncConfig config;
  config.backend = "git";
  config.remote_url = bare_url;
  config.interval_seconds = 300;
  return f.backend.Initialize(f.notebook_root, config);
}

void teardown_fixture(Fixture &f) {
  cleanup_test_dir(f.bare_path);
  cleanup_test_dir(f.notebook_root);
  cleanup_test_dir(f.clone_dir);
}

// Clone the bare repo at <bare_path> into a fresh non-bare working tree at
// <dest_dir>. Avoids git_clone() because libgit2's git_clone over file://
// URLs hangs on Windows in this codebase (see comments in
// test_git_sync_init.cpp and test_git_sync_helpers.cpp). Instead reproduces
// the equivalent: init + add origin + fetch + checkout origin/main +
// bind refs/heads/main. Throws on failure.
void clone_bare_to_workdir(const std::string &bare_path,
                           const std::string &dest_dir) {
  vxcore::LibGit2Init guard;

  // Build a file:// URL for the bare repo with forward slashes.
  std::error_code ec;
  std::filesystem::path bare_abs =
      std::filesystem::absolute(vxcore::PathFromUtf8(bare_path), ec);
  std::string bare_abs_str = ec ? bare_path : bare_abs.string();
  std::string url = std::string("file:///") + bare_abs_str;
  for (auto &c : url) {
    if (c == '\\') c = '/';
  }
  if (url.compare(0, 9, "file:////") == 0) {
    url.erase(7, 1);
  }

  // Init a fresh non-bare repo at dest_dir with initial HEAD on main.
  git_repository *repo = nullptr;
  git_repository_init_options iopts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
  if (git_repository_init_options_init(
          &iopts, GIT_REPOSITORY_INIT_OPTIONS_VERSION) != 0) {
    throw std::runtime_error("clone_bare: init_options_init");
  }
  iopts.flags = GIT_REPOSITORY_INIT_MKDIR | GIT_REPOSITORY_INIT_MKPATH;
  iopts.initial_head = "main";
  if (git_repository_init_ext(&repo, dest_dir.c_str(), &iopts) != 0) {
    const git_error *e = git_error_last();
    throw std::runtime_error(std::string("clone_bare: init_ext: ") +
                             ((e && e->message) ? e->message : "unknown"));
  }

  // Create the origin remote pointing at the bare's file:// URL.
  git_remote *origin = nullptr;
  if (git_remote_create(&origin, repo, "origin", url.c_str()) != 0) {
    const git_error *e = git_error_last();
    git_repository_free(repo);
    throw std::runtime_error(std::string("clone_bare: remote_create: ") +
                             ((e && e->message) ? e->message : "unknown"));
  }

  // Fetch all refs.
  git_fetch_options fopts = GIT_FETCH_OPTIONS_INIT;
  if (git_remote_fetch(origin, nullptr, &fopts, "gitkeep roundtrip fetch") !=
      0) {
    const git_error *e = git_error_last();
    git_remote_free(origin);
    git_repository_free(repo);
    throw std::runtime_error(std::string("clone_bare: remote_fetch: ") +
                             ((e && e->message) ? e->message : "unknown"));
  }
  git_remote_free(origin);

  // Resolve refs/remotes/origin/main; fall back to master if missing.
  git_reference *origin_ref = nullptr;
  int rc =
      git_reference_lookup(&origin_ref, repo, "refs/remotes/origin/main");
  if (rc != 0) {
    git_error_clear();
    rc = git_reference_lookup(&origin_ref, repo, "refs/remotes/origin/master");
  }
  if (rc != 0) {
    git_repository_free(repo);
    throw std::runtime_error("clone_bare: no origin/main or origin/master ref");
  }

  git_object *target = nullptr;
  if (git_reference_peel(&target, origin_ref, GIT_OBJECT_COMMIT) != 0) {
    git_reference_free(origin_ref);
    git_repository_free(repo);
    throw std::runtime_error("clone_bare: reference_peel");
  }

  // Force-checkout the remote tree into the workdir.
  git_checkout_options copts = GIT_CHECKOUT_OPTIONS_INIT;
  copts.checkout_strategy = GIT_CHECKOUT_FORCE;
  if (git_checkout_tree(repo, target, &copts) != 0) {
    git_object_free(target);
    git_reference_free(origin_ref);
    git_repository_free(repo);
    throw std::runtime_error("clone_bare: checkout_tree");
  }

  // Bind local refs/heads/main to the same OID so HEAD resolves.
  git_oid head_oid;
  git_oid_cpy(&head_oid, git_object_id(target));
  git_object_free(target);
  git_reference_free(origin_ref);

  git_reference *new_branch_ref = nullptr;
  if (git_reference_create(&new_branch_ref, repo, "refs/heads/main", &head_oid,
                           /*force=*/1, "gitkeep roundtrip clone") != 0) {
    git_repository_free(repo);
    throw std::runtime_error("clone_bare: reference_create(main)");
  }
  git_reference_free(new_branch_ref);

  if (git_repository_set_head(repo, "refs/heads/main") != 0) {
    git_repository_free(repo);
    throw std::runtime_error("clone_bare: set_head(main)");
  }

  git_repository_free(repo);
}

// Recursively walk <root> and return true iff any regular file named .gitkeep
// is found. Returns false (without throwing) if <root> does not exist.
bool any_gitkeep_under(const std::string &root) {
  std::error_code ec;
  auto root_path = vxcore::PathFromUtf8(root);
  if (!std::filesystem::exists(root_path, ec)) return false;
  for (auto it = std::filesystem::recursive_directory_iterator(
           root_path,
           std::filesystem::directory_options::skip_permission_denied, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
    std::error_code fec;
    if (it->is_regular_file(fec) && it->path().filename() == ".gitkeep") {
      return true;
    }
  }
  return false;
}

// The one test in this file: an empty user folder must survive a full
// Sync() + clone-elsewhere cycle, with the .gitkeep marker present in the
// clone and the skip-listed trees free of any marker.
int test_empty_folder_survives_push_and_clone() {
  std::cout
      << "  Running test_empty_folder_survives_push_and_clone..." << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "survives"), VXCORE_OK);

    // Empty user folder — the whole point of the feature.
    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/MyEmpty"));

    // Full round-trip: stage + commit + fetch + rebase + push.
    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    // Clone the bare into a completely separate directory.
    clone_bare_to_workdir(f.bare_path, f.clone_dir);

    // The empty folder must have survived the round-trip with its .gitkeep.
    ASSERT_TRUE(
        std::filesystem::exists(vxcore::PathFromUtf8(f.clone_dir + "/MyEmpty")));
    ASSERT_TRUE(std::filesystem::exists(
        vxcore::PathFromUtf8(f.clone_dir + "/MyEmpty/.gitkeep")));
    {
      std::error_code ec;
      auto sz = std::filesystem::file_size(
          vxcore::PathFromUtf8(f.clone_dir + "/MyEmpty/.gitkeep"), ec);
      ASSERT_FALSE(static_cast<bool>(ec));
      ASSERT_EQ(sz, static_cast<std::uintmax_t>(0));
    }

    // No marker at the notebook root level.
    ASSERT_FALSE(
        std::filesystem::exists(vxcore::PathFromUtf8(f.clone_dir + "/.gitkeep")));

    // vx_notebook/ and vx_recycle_bin/ are skip-listed at the root; even if
    // they happen to appear in the clone, no .gitkeep anywhere beneath them.
    ASSERT_FALSE(any_gitkeep_under(f.clone_dir + "/vx_notebook"));
    ASSERT_FALSE(any_gitkeep_under(f.clone_dir + "/vx_recycle_bin"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_empty_folder_survives_push_and_clone passed"
            << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  RUN_TEST(test_empty_folder_survives_push_and_clone);
  std::cout << "All test_gitkeep_roundtrip tests passed" << std::endl;
  return 0;
}
