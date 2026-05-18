#ifndef VXCORE_SYNC_GIT_GIT_CONFIG_FIXER_H
#define VXCORE_SYNC_GIT_GIT_CONFIG_FIXER_H

#include <string>

namespace vxcore {

// Pre-open fix: if a previously-bootstrapped gitdir has a stale
// core.worktree (e.g., user moved the notebook folder on disk), libgit2's
// git_repository_open will fail with GIT_ENOTFOUND on the stale path.
// Rewrite the config file directly via std::filesystem BEFORE handing it
// to libgit2. Idempotent — writing the same value is harmless.
//
// git config file is a plain INI text file. We do a line-by-line scan
// and replace any `worktree = ...` line under the [core] section.
//
// Failures are logged via VXCORE_LOG_WARN and the function returns;
// libgit2 may still succeed or surface a more useful error.
void RebindCoreWorktree(const std::string &root_folder);

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_CONFIG_FIXER_H
