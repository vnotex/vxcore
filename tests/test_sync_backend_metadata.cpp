// Task 4.1 of sync-backend-phase4 (F1.2): verify the new ISyncBackend
// identity methods (GetName / GetCapabilities / IsInitialized) on both the
// concrete GitSyncBackend and the configurable MockSyncBackend.
//
// Compile pattern mirrors test_git_sync_init / test_libgit2_init: we
// direct-compile git_sync_backend.cpp + its transitive deps so the test
// binary owns its own GitSyncBackend + libgit2 ref-count statics, avoiding
// the Windows DLL static-storage duplication problem. We do NOT link
// vxcore.dll for this binary, hence the no-op vxcore_set_test_mode shim.

#include <git2.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "sync/git/git_sync_backend.h"
#include "sync/git/libgit2_init.h"
#include "sync/sync_backend.h"
#include "sync/sync_types.h"
#include "test_git_sync_helpers.h"
#include "test_internals/mock_sync_backend.h"
#include "test_utils.h"
#include "vxcore/vxcore_types.h"

namespace {

// Per libs/vxcore/AGENTS.md "Standalone test pattern" — a no-op shim so the
// vxcore_set_test_mode() call below links without dragging in vxcore.dll.
extern "C" void vxcore_set_test_mode(int) {}

bool HasBit(vxcore::SyncCapabilities caps, vxcore::SyncCapability bit) {
  return (caps & static_cast<uint32_t>(bit)) != 0;
}

// Build a notebook root whose vx_notebook/vx_sync/ is a fresh libgit2 repo
// (no remote refs needed for these metadata tests — Initialize will just
// take the OpenExistingRepo branch). Uses NO_DOTGIT_DIR to put the gitdir
// literally at <root>/vx_notebook/vx_sync (matching GitSyncBackend's layout).
std::string seed_notebook_with_fresh_repo(const std::string &label,
                                          const std::string &origin_url) {
  const std::string base = (std::filesystem::temp_directory_path() /
                            ("vxcore_test_metadata_" + label))
                               .string();
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base);

  vxcore::LibGit2Init guard;

  const std::string git_dir = base + "/vx_notebook/vx_sync";

  git_repository_init_options opts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
  if (git_repository_init_options_init(
          &opts, GIT_REPOSITORY_INIT_OPTIONS_VERSION) < 0) {
    throw std::runtime_error("init_options_init failed");
  }
  opts.flags = GIT_REPOSITORY_INIT_MKDIR | GIT_REPOSITORY_INIT_MKPATH |
               GIT_REPOSITORY_INIT_NO_DOTGIT_DIR;
  opts.workdir_path = base.c_str();
  opts.initial_head = "main";
  opts.origin_url = origin_url.c_str();

  git_repository *repo = nullptr;
  if (git_repository_init_ext(&repo, git_dir.c_str(), &opts) != 0) {
    const git_error *e = git_error_last();
    throw std::runtime_error(std::string("repo init: ") +
                             (e && e->message ? e->message : "?"));
  }
  git_repository_free(repo);

  return base;
}

int git_name_is_git() {
  std::cout << "  Running git_name_is_git..." << std::endl;
  const std::string url = vxcore_test::create_bare_repo("metadata_name");
  const std::string root = seed_notebook_with_fresh_repo("name", url);

  vxcore::GitSyncBackend backend;
  vxcore::SyncConfig cfg;
  cfg.backend = "git";
  cfg.remote_url = url;
  ASSERT_EQ(backend.Initialize(root, cfg), VXCORE_OK);

  ASSERT_EQ(backend.GetName(), std::string("git"));

  // Wave 4.5 removed Shutdown(); teardown is via destructor at scope exit.
  std::filesystem::remove_all(root);
  std::cout << "  [PASS] git_name_is_git" << std::endl;
  return 0;
}

int git_caps_include_conflict_detection_and_resolution() {
  std::cout << "  Running git_caps_include_conflict_detection_and_resolution..."
            << std::endl;
  vxcore::GitSyncBackend backend;
  const auto caps = backend.GetCapabilities();
  ASSERT_TRUE(HasBit(caps, vxcore::SyncCapability::ConflictDetection));
  ASSERT_TRUE(HasBit(caps, vxcore::SyncCapability::ConflictResolution));
  ASSERT_TRUE(HasBit(caps, vxcore::SyncCapability::IncrementalSync));
  ASSERT_TRUE(HasBit(caps, vxcore::SyncCapability::AuthRequired));
  ASSERT_TRUE(HasBit(caps, vxcore::SyncCapability::ProgressReporting));
  std::cout << "  [PASS] git_caps_include_conflict_detection_and_resolution"
            << std::endl;
  return 0;
}

int git_caps_do_not_include_cancellation() {
  std::cout << "  Running git_caps_do_not_include_cancellation..." << std::endl;
  vxcore::GitSyncBackend backend;
  // Wave 12 of sync-backend-phase4 will flip this bit on; until then, the
  // backend MUST NOT advertise cooperative cancellation.
  ASSERT_FALSE(HasBit(backend.GetCapabilities(),
                      vxcore::SyncCapability::Cancellation));
  std::cout << "  [PASS] git_caps_do_not_include_cancellation" << std::endl;
  return 0;
}

int git_init_state_transitions() {
  std::cout << "  Running git_init_state_transitions..." << std::endl;
  const std::string url = vxcore_test::create_bare_repo("metadata_init");
  const std::string root = seed_notebook_with_fresh_repo("init", url);

  {
    vxcore::GitSyncBackend backend;
    ASSERT_FALSE(backend.IsInitialized());

    vxcore::SyncConfig cfg;
    cfg.backend = "git";
    cfg.remote_url = url;
    ASSERT_EQ(backend.Initialize(root, cfg), VXCORE_OK);
    ASSERT_TRUE(backend.IsInitialized());
  }
  // Wave 4.5 removed Shutdown(); the destructor at scope exit now resets
  // initialized_ to false. We can't observe the post-teardown IsInitialized()
  // value without the object existing, so the contract we verify is that the
  // destructor runs without crashing on a fully-initialized backend.

  std::filesystem::remove_all(root);
  std::cout << "  [PASS] git_init_state_transitions" << std::endl;
  return 0;
}

int mock_configurable_name_caps_init() {
  std::cout << "  Running mock_configurable_name_caps_init..." << std::endl;
  vxcore::MockSyncBackend mock;

  // Defaults.
  ASSERT_EQ(mock.GetName(), std::string("mock"));
  ASSERT_EQ(mock.GetCapabilities(),
            static_cast<vxcore::SyncCapabilities>(
                vxcore::SyncCapability::None));
  ASSERT_TRUE(mock.IsInitialized());

  // Setters.
  mock.SetName("fake-backend");
  ASSERT_EQ(mock.GetName(), std::string("fake-backend"));

  const auto caps = static_cast<vxcore::SyncCapabilities>(
                        vxcore::SyncCapability::ConflictDetection) |
                    static_cast<vxcore::SyncCapabilities>(
                        vxcore::SyncCapability::ProgressReporting);
  mock.SetCapabilities(caps);
  ASSERT_EQ(mock.GetCapabilities(), caps);

  mock.SetInitialized(false);
  ASSERT_FALSE(mock.IsInitialized());

  std::cout << "  [PASS] mock_configurable_name_caps_init" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  std::cout << "Running ISyncBackend metadata tests (Task 4.1)..." << std::endl;

  int result = 0;
  result |= git_name_is_git();
  result |= git_caps_include_conflict_detection_and_resolution();
  result |= git_caps_do_not_include_cancellation();
  result |= git_init_state_transitions();
  result |= mock_configurable_name_caps_init();

  if (result == 0) {
    std::cout << "All metadata tests passed!" << std::endl;
  }
  return result;
}
