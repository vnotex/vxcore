// T13 of open-notebook-remote-readonly plan. Tests for GitSyncBackend::Clone,
// the one-shot wrapper around BootstrapFromEmptyRemote that lets a caller
// materialize a notebook on disk from a remote sync URL.
//
// Subtests:
//   1. Happy path — clone from a file:// bare-repo fixture into an empty
//      target directory; assert VXCORE_OK and that <target>/vx_notebook/
//      vx_sync/HEAD exists post-clone (proves libgit2 wrote a real gitdir).
//   2. Validation — Clone with an empty remote_url returns
//      VXCORE_ERR_INVALID_PARAM and writes nothing to disk.
//   3. Failure surface — Clone against a nonexistent remote URL returns
//      either VXCORE_ERR_SYNC_NETWORK or VXCORE_ERR_UNKNOWN (per the
//      mapping in git_error_translator.cpp); target dir may have a partial
//      vx_notebook/vx_sync/ but the operation must NOT crash and the error
//      code must be non-OK.
//
// Mirrors the fixture pattern from test_gitkeep_roundtrip.cpp: this binary
// direct-compiles git_sync_backend.cpp (and its transitive deps) rather than
// linking vxcore.dll, so it owns its own GitSyncBackend + libgit2 ref-count
// statics. Same Windows-DLL static-storage workaround used by every other
// test_git_sync_* / test_gitkeep_* target.

#include <git2.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "sync/git/git_sync_backend.h"
#include "sync/git/libgit2_init.h"
#include "sync/sync_backend.h"
#include "sync/sync_types.h"
#include "test_git_sync_helpers.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore_types.h"

namespace {

// vxcore_set_test_mode lives in vxcore.dll which is NOT linked into this test
// binary (we direct-compile git_sync_backend.cpp + its transitive deps).
// get_test_path() already places everything under %TEMP%, so the production
// test-mode flag is moot here. Provide a local no-op stub so the mandated
// vxcore_set_test_mode(1) call in main() links cleanly. Anonymous namespace
// prevents collision with the real C-linkage symbol if anyone ever links
// vxcore.dll into a sibling test.
void vxcore_set_test_mode(int /*enabled*/) {}

// Subtest 1: happy path. Clone from a file:// bare-repo fixture into an empty
// target directory; assert VXCORE_OK and that the canonical gitdir exists
// at <target>/vx_notebook/vx_sync/HEAD.
int test_clone_happy_path_from_bare_fixture() {
  std::cout << "  Running test_clone_happy_path_from_bare_fixture..." << std::endl;

  const std::string bare_path = get_test_path("git_sync_clone_happy_bare");
  const std::string target_dir = get_test_path("git_sync_clone_happy_target");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(target_dir);

  // Seed bare repo with one commit so refs/remotes/origin/main exists post-fetch.
  const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
  vxcore_test::commit_file(bare_path, "seed.md", "seed body", "init commit");

  // Pre-create the empty target_dir (caller's contract per ISyncBackend::Clone).
  create_directory(target_dir);

  vxcore::SyncConfig config;
  config.backend = "git";
  config.remote_url = bare_url;
  config.interval_seconds = 300;

  vxcore::GitSyncBackend backend;
  VxCoreError err = backend.Clone(target_dir, config);

  ASSERT_EQ(err, VXCORE_OK);

  // Canonical gitdir layout produced by BootstrapFromEmptyRemote:
  //   <target>/vx_notebook/vx_sync/HEAD
  const std::string head_path = target_dir + "/vx_notebook/vx_sync/HEAD";
  ASSERT_TRUE(path_exists(head_path));

  // Working tree must contain the seed file (proves the fetch+checkout worked).
  ASSERT_TRUE(path_exists(target_dir + "/seed.md"));

  // Backend must NOT have become "initialized" — Clone is one-shot transient.
  ASSERT_FALSE(backend.IsInitialized());

  cleanup_test_dir(bare_path);
  cleanup_test_dir(target_dir);
  std::cout << "  test_clone_happy_path_from_bare_fixture passed" << std::endl;
  return 0;
}

// Subtest 2: empty remote_url → VXCORE_ERR_INVALID_PARAM, nothing on disk.
int test_clone_rejects_empty_remote_url() {
  std::cout << "  Running test_clone_rejects_empty_remote_url..." << std::endl;

  const std::string target_dir = get_test_path("git_sync_clone_empty_url_target");
  cleanup_test_dir(target_dir);
  create_directory(target_dir);

  vxcore::SyncConfig config;
  config.backend = "git";
  config.remote_url = "";  // the validation trigger
  config.interval_seconds = 300;

  vxcore::GitSyncBackend backend;
  VxCoreError err = backend.Clone(target_dir, config);

  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);

  // The vx_notebook/vx_sync/ subtree must NOT have been created — validation
  // happens BEFORE any filesystem op.
  ASSERT_FALSE(path_exists(target_dir + "/vx_notebook"));
  ASSERT_FALSE(path_exists(target_dir + "/vx_notebook/vx_sync"));

  cleanup_test_dir(target_dir);
  std::cout << "  test_clone_rejects_empty_remote_url passed" << std::endl;
  return 0;
}

// Subtest 3: nonexistent remote → VXCORE_ERR_SYNC_NETWORK or VXCORE_ERR_UNKNOWN
// (per git_error_translator.cpp mapping for failed file:// fetch). Operation
// must NOT crash; backend stays uninitialized.
int test_clone_fails_on_nonexistent_remote() {
  std::cout << "  Running test_clone_fails_on_nonexistent_remote..." << std::endl;

  const std::string target_dir =
      get_test_path("git_sync_clone_bad_remote_target");
  const std::string nonexistent_bare =
      get_test_path("git_sync_clone_bad_remote_bare_DOES_NOT_EXIST");
  cleanup_test_dir(target_dir);
  cleanup_test_dir(nonexistent_bare);
  create_directory(target_dir);

  // Build a file:// URL pointing at a path that does NOT exist on disk.
  std::error_code ec;
  std::filesystem::path bare_abs =
      std::filesystem::absolute(vxcore::PathFromUtf8(nonexistent_bare), ec);
  std::string bare_abs_str = ec ? nonexistent_bare : bare_abs.string();
  std::string url = std::string("file:///") + bare_abs_str;
  for (auto &c : url) {
    if (c == '\\') c = '/';
  }
  if (url.compare(0, 9, "file:////") == 0) {
    url.erase(7, 1);
  }

  vxcore::SyncConfig config;
  config.backend = "git";
  config.remote_url = url;
  config.interval_seconds = 300;

  vxcore::GitSyncBackend backend;
  VxCoreError err = backend.Clone(target_dir, config);

  // Per git_error_translator.cpp: GIT_ERROR_NET / GIT_ERROR_SSL / GIT_ERROR_SSH
  // → VXCORE_ERR_SYNC_NETWORK; anything else falls through to
  // VXCORE_ERR_UNKNOWN. file:// "no such repo" most commonly classifies as
  // VXCORE_ERR_UNKNOWN on Windows because libgit2 surfaces the local-transport
  // failure without a network klass. Either is acceptable.
  ASSERT_TRUE(err == VXCORE_ERR_SYNC_NETWORK || err == VXCORE_ERR_UNKNOWN);
  ASSERT_NE(err, VXCORE_OK);

  // Backend must NOT have become "initialized" on the failure path.
  ASSERT_FALSE(backend.IsInitialized());

  cleanup_test_dir(target_dir);
  cleanup_test_dir(nonexistent_bare);
  std::cout << "  test_clone_fails_on_nonexistent_remote passed" << std::endl;
  return 0;
}

// Sanity check: capability bit advertisement matches the implementation.
// (Anchors T6's SyncCapability::Cloneable contract for this backend.)
int test_clone_capability_bit_set() {
  std::cout << "  Running test_clone_capability_bit_set..." << std::endl;
  vxcore::GitSyncBackend backend;
  const vxcore::SyncCapabilities caps = backend.GetCapabilities();
  ASSERT_TRUE((caps & static_cast<uint32_t>(vxcore::SyncCapability::Cloneable)) != 0);
  std::cout << "  test_clone_capability_bit_set passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  RUN_TEST(test_clone_capability_bit_set);
  RUN_TEST(test_clone_rejects_empty_remote_url);
  RUN_TEST(test_clone_happy_path_from_bare_fixture);
  RUN_TEST(test_clone_fails_on_nonexistent_remote);
  std::cout << "All test_git_sync_clone tests passed" << std::endl;
  return 0;
}
