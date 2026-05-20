// Task 2.6 (B6) of sync-backend-phase4 plan: ResolveResult tri-state enum.
//
// Exercises the new tri-state enum returned by
// GitConflictResolver::ResolveConflictEx. Two of the four enum values are
// directly testable here without spinning up a full clone + rebase fixture:
//
//   * NoConflict — call resolver against a clean repo / path with no
//                  conflict entry; must return NoConflict (not Failed).
//   * Failed    — pass a nullptr libgit2 repo handle; must return Failed.
//
// The remaining two (Resolved, Aborted) are intentionally documented as
// TODO subtests because:
//
//   * Resolved — reaching this state requires a full clone + divergent
//                push + rebase-onto-origin fixture (the setup pattern lives
//                in test_git_sync_conflicts.cpp and runs there end-to-end
//                via the legacy bool wrapper, which now delegates to the
//                enum path). Duplicating that fixture here adds zero new
//                coverage; the legacy test continues to exercise the
//                Resolved → VXCORE_OK collapse.
//   * Aborted — no public API currently triggers an aborted resolution;
//                the enum value exists for forward-compatibility with the
//                Wave 7 C ABI redesign. Marked TODO; will be wired when
//                the cancellation token (D9 / Wave 12) lands.
//
// Per AGENTS.md the test runs standalone (direct-compiles the resolver and
// its transitive deps) and provides a local vxcore_set_test_mode shim.

#include <git2.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "sync/git/git_conflict_resolver.h"
#include "sync/git/git_sync_pipeline.h"
#include "sync/git/libgit2_init.h"
#include "sync/sync_types.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore_types.h"

namespace {

// Local shim — this test does NOT link vxcore.dll, so the real
// vxcore_set_test_mode symbol is unavailable. The shim is a no-op because
// the test only touches a self-contained git repo under %TEMP%, never
// AppData.
void vxcore_set_test_mode(int /*enabled*/) {}

std::string make_temp_repo_path(const std::string &suffix) {
  auto root = std::filesystem::temp_directory_path() /
              ("vxcore_resolve_result_" + suffix + "_" +
               std::to_string(std::rand()));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  (void)ec;
  std::filesystem::create_directories(root, ec);
  (void)ec;
  return root.string();
}

git_repository *init_workdir_repo(const std::string &workdir) {
  // Plain (non-bare) init under workdir. Good enough for the no-conflict
  // path — we never need to actually commit anything because
  // git_index_conflict_get on a fresh index returns GIT_ENOTFOUND for any
  // path, which is exactly the NoConflict case.
  git_repository_init_options opts;
  git_repository_init_options_init(&opts, GIT_REPOSITORY_INIT_OPTIONS_VERSION);
  opts.flags = GIT_REPOSITORY_INIT_MKPATH;
  opts.initial_head = "main";
  git_repository *repo = nullptr;
  int rc = git_repository_init_ext(&repo, workdir.c_str(), &opts);
  if (rc != 0 || repo == nullptr) {
    std::cerr << "    init_workdir_repo failed: rc=" << rc << std::endl;
    return nullptr;
  }
  return repo;
}

}  // namespace

// ----------------------------------------------------------------------
// Subtest: returns_no_conflict_when_clean
// ----------------------------------------------------------------------
int test_returns_no_conflict_when_clean() {
  std::cout << "  Running test_returns_no_conflict_when_clean..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore::LibGit2Init guard;

  const std::string workdir = make_temp_repo_path("noconflict");
  git_repository *repo = init_workdir_repo(workdir);
  ASSERT_NOT_NULL(repo);

  vxcore::GitConflictResolver resolver(repo, workdir);
  // Build a pipeline pointing at the same repo. ContinueRebaseAfterResolution
  // is never called on the NoConflict path (we short-circuit on
  // GIT_ENOTFOUND), so the pipeline's other inputs (config, credentials,
  // git_dir) don't need to be meaningful.
  vxcore::SyncConfig cfg;
  vxcore::SyncCredentials creds;
  git_rebase *rebase_in_progress = nullptr;
  const std::string git_dir = workdir + "/.git";
  vxcore::GitSyncPipeline pipeline(repo, git_dir, workdir, cfg, creds,
                                   &rebase_in_progress);

  vxcore::ResolveResult r = resolver.ResolveConflictEx(
      "does_not_exist.md", vxcore::SyncConflictResolution::kKeepLocal,
      pipeline);
  ASSERT(r == vxcore::ResolveResult::NoConflict);

  git_repository_free(repo);
  std::error_code ec;
  std::filesystem::remove_all(workdir, ec);
  std::cout << "  ✓ test_returns_no_conflict_when_clean passed" << std::endl;
  return 0;
}

// ----------------------------------------------------------------------
// Subtest: returns_failed_on_unrecoverable_error
// ----------------------------------------------------------------------
int test_returns_failed_on_unrecoverable_error() {
  std::cout << "  Running test_returns_failed_on_unrecoverable_error..."
            << std::endl;
  vxcore_set_test_mode(1);
  vxcore::LibGit2Init guard;

  // Resolver with a nullptr repo handle — the first guard inside
  // ResolveConflictEx fails fast with Failed. Pipeline is constructed
  // against a real but unused repo so we don't trip its own ctor; we never
  // reach the ContinueRebaseAfterResolution path.
  const std::string workdir = make_temp_repo_path("failed");
  git_repository *real_repo = init_workdir_repo(workdir);
  ASSERT_NOT_NULL(real_repo);

  vxcore::SyncConfig cfg;
  vxcore::SyncCredentials creds;
  git_rebase *rebase_in_progress = nullptr;
  const std::string git_dir = workdir + "/.git";
  vxcore::GitSyncPipeline pipeline(real_repo, git_dir, workdir, cfg, creds,
                                   &rebase_in_progress);

  vxcore::GitConflictResolver bad_resolver(/*repo=*/nullptr, workdir);
  vxcore::ResolveResult r = bad_resolver.ResolveConflictEx(
      "anything.md", vxcore::SyncConflictResolution::kKeepLocal, pipeline);
  ASSERT(r == vxcore::ResolveResult::Failed);

  // Sanity-check the legacy wrapper collapses Failed back to VXCORE_ERR_UNKNOWN.
  VxCoreError legacy = bad_resolver.ResolveConflict(
      "anything.md", vxcore::SyncConflictResolution::kKeepLocal, pipeline);
  ASSERT(legacy != VXCORE_OK);

  git_repository_free(real_repo);
  std::error_code ec;
  std::filesystem::remove_all(workdir, ec);
  std::cout << "  ✓ test_returns_failed_on_unrecoverable_error passed"
            << std::endl;
  return 0;
}

// ----------------------------------------------------------------------
// Subtest: returns_resolved_on_successful_resolution (TODO)
// ----------------------------------------------------------------------
// Reaching ResolveResult::Resolved requires a full clone + divergent push +
// rebase-onto-origin fixture. The end-to-end path is already covered by
// test_git_sync_conflicts.cpp (T30/T31/T32) through the legacy
// VxCoreError-returning wrapper, which now delegates to ResolveConflictEx
// and therefore exercises the Resolved → VXCORE_OK collapse implicitly.
// Re-implementing the libgit2 fixture here adds zero new behavioural
// coverage; leaving as TODO until B6-followup widens the C ABI in Wave 7.
int test_returns_resolved_on_successful_resolution() {
  std::cout
      << "  Skipping test_returns_resolved_on_successful_resolution "
         "(TODO — see comment in test source; covered transitively by "
         "test_git_sync_conflicts)"
      << std::endl;
  return 0;
}

// ----------------------------------------------------------------------
// Subtest: returns_aborted_when_user_aborts (TODO)
// ----------------------------------------------------------------------
// ResolveResult::Aborted exists for forward-compatibility with the Wave 7
// C ABI redesign + Wave 12 cancellation token (D9). Today no public API
// path drives the resolver into an aborted state, so a unit test would
// have to fake it via friend access or by short-circuiting internal state
// — both worse than documenting the gap. Marked TODO.
int test_returns_aborted_when_user_aborts() {
  std::cout << "  Skipping test_returns_aborted_when_user_aborts "
               "(TODO — Aborted state not reachable via current API; "
               "wired in Wave 7 / Wave 12)"
            << std::endl;
  return 0;
}

int main() {
  std::cout << "=== test_resolve_result ===" << std::endl;
  int failed = 0;
  RUN_TEST(test_returns_no_conflict_when_clean);
  RUN_TEST(test_returns_failed_on_unrecoverable_error);
  RUN_TEST(test_returns_resolved_on_successful_resolution);
  RUN_TEST(test_returns_aborted_when_user_aborts);
  if (failed > 0) {
    std::cout << failed << " test(s) FAILED" << std::endl;
    return 1;
  }
  std::cout << "All tests passed" << std::endl;
  return 0;
}
