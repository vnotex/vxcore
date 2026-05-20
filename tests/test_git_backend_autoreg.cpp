// Task 4.3 (sync-backend-phase4 F1.1): verify GitSyncBackend self-registers
// into SyncBackendRegistry via its anonymous-namespace BackendRegistration
// token, and that the EnsureGitBackendLinked anchor (called by LibGit2Init's
// ctor) is sufficient to keep the registration alive even when nothing else
// directly references git_sync_backend.cpp.
//
// Compile pattern mirrors test_sync_backend_metadata: direct-compile
// git_sync_backend.cpp + its transitive deps (centralized in
// VXCORE_GIT_SYNC_SOURCES_ABS via the CMakeLists block below) and link
// git_sync_test_helpers for libgit2_init/logger/file_utils. Do NOT link
// vxcore.dll — this test owns its own GitSyncBackend + registry statics.

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "sync/git/libgit2_init.h"
#include "sync/sync_backend_registry.h"
#include "test_utils.h"

namespace {

// Per libs/vxcore/AGENTS.md "Standalone test pattern" — a no-op shim so the
// vxcore_set_test_mode() call links without dragging in vxcore.dll.
extern "C" void vxcore_set_test_mode(int) {}

int git_backend_registered_after_libgit2init() {
  std::cout << "  Running git_backend_registered_after_libgit2init..." << std::endl;
  // Construct LibGit2Init: per Task 4.3, its ctor calls the cross-TU anchor
  // (g_git_backend_link_anchor) which references EnsureGitBackendLinked,
  // forcing the linker to keep git_sync_backend.cpp's BackendRegistration
  // alive. The static-init token has already run by main() in any case, but
  // creating the guard exercises the anchor codepath.
  vxcore::LibGit2Init guard;

  auto &r = vxcore::SyncBackendRegistry::Instance();
  auto names = r.Names();
  std::cout << "  Registered backends:";
  for (const auto &n : names) std::cout << " " << n;
  std::cout << std::endl;

  bool has_git = std::find(names.begin(), names.end(), std::string("git")) !=
                 names.end();
  ASSERT_TRUE(has_git);

  // Also verify Create() returns a non-null instance for "git".
  vxcore::SyncConfig cfg;
  cfg.backend = "git";
  auto inst = r.Create("git", cfg, nullptr);
  ASSERT_NOT_NULL(inst.get());
  ASSERT_EQ(inst->GetName(), std::string("git"));

  std::cout << "  PASS" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  std::cout << "Running GitSyncBackend auto-registration tests..." << std::endl;
  RUN_TEST(git_backend_registered_after_libgit2init);
  std::cout << "All git_backend_autoreg tests passed." << std::endl;
  return 0;
}
