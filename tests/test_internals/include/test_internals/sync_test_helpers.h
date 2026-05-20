#ifndef VXCORE_TEST_INTERNALS_SYNC_TEST_HELPERS_H
#define VXCORE_TEST_INTERNALS_SYNC_TEST_HELPERS_H

// Minimal shared test helpers for the vxcore sync test suite.
//
// This header is the public surface of the `vxcore_test_internals` STATIC
// library (Task 0.1 of `.sisyphus/plans/sync-backend-phase4.md`). It is the
// long-term replacement for the inline test_git_sync_helpers.{h,cpp} duo;
// later Wave-0 tasks will migrate existing test binaries onto it. For now,
// we keep the surface intentionally small — just enough to prove the
// scaffolding works end-to-end.
//
// Rules (see libs/vxcore/AGENTS.md for full rationale):
//   - Pure C++17 + libgit2 + std. NO Qt.
//   - All path strings are UTF-8 (Windows-safe; wrap with PathFromUtf8 at
//     std::filesystem boundaries).
//   - Helpers throw std::runtime_error on libgit2 failure; tests can let
//     that propagate to mark the run as FAIL.

#include <git2.h>

#include <string>
#include <vector>

namespace vxcore_test_internals {

// Create a fresh bare repository (initial_head = "main") at <bare_path>.
// Any pre-existing directory at the target is removed first so tests can
// be re-run. Parent directories are created on demand.
// Returns the file:// URL of the bare repo, suitable for use as a libgit2
// remote URL (e.g. "file:///C:/temp/foo.git").
std::string MakeBareRemote(const std::string& bare_path);

// Create a fresh empty workspace directory under %TEMP% / $TMPDIR.
// The path will not exist until the call returns; the directory is created.
// A unique suffix is appended so concurrent tests do not collide.
// Returns the absolute UTF-8 path.
std::string MakeTempWorkspace();

// Build a libgit2 signature with author "VNote Test"
// <vnote-test@vnote.local> and the current wall-clock time. The caller is
// responsible for git_signature_free(). Returns nullptr on libgit2 error.
git_signature* CreateTestSignature();

// Run a libgit2 command-equivalent shell-out is intentionally avoided —
// vxcore tests do not depend on an external git binary. Instead this helper
// executes a libgit2-backed "command" by interpreting <args> as a small DSL:
//   {"init", "<path>"}            -> git_repository_init (non-bare)
//   {"init", "--bare", "<path>"}  -> git_repository_init (bare, main)
// Returns 0 on success and a non-zero libgit2 error code otherwise. Tests
// that need richer command coverage should use libgit2 directly; this
// helper exists only to give the scaffolding a stable, minimal entry
// point that callers can grow without re-shaping the library API.
int RunGitCommand(const std::vector<std::string>& args);

}  // namespace vxcore_test_internals

#endif  // VXCORE_TEST_INTERNALS_SYNC_TEST_HELPERS_H
