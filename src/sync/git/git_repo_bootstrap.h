#ifndef VXCORE_SYNC_GIT_GIT_REPO_BOOTSTRAP_H
#define VXCORE_SYNC_GIT_GIT_REPO_BOOTSTRAP_H

#include <string>

namespace vxcore {

// Remove the libgit2-managed `.git` gitlink file from the workdir.
//
// libgit2 writes a `.git` gitlink file in the workdir when the gitdir lives
// elsewhere (so plain `git` CLI can find it). VNote doesn't want notebooks
// to look like git working trees to external tools, so we remove the
// gitlink — vx_notebook/vx_sync/ remains the canonical gitdir and
// GitSyncBackend re-open uses git_repository_open on it directly.
//
// Only deletes a regular file (the gitlink). Never deletes a real user
// `.git` directory.
//
// Failures are logged via VXCORE_LOG_WARN with the supplied |context|
// prefix (e.g., "Initialize(clone)" or "Initialize(initpush)") so the log
// output is byte-identical to the inlined sites this helper replaces.
//
// Note: this is the first step of T12 (git_repo_bootstrap extraction). The
// remaining state-machine helpers (OpenExistingRepo, BootstrapFromEmptyRemote,
// BootstrapToEmptyRemote, InitEmptyRepoWithRemote) are deferred per the
// shell-blocker constraint documented in
// .sisyphus/notepads/git-folder-restructure/issues.md — full Initialize
// refactor requires build verification capability.
void ScrubGitlink(const std::string &root_folder, const std::string &context);

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_REPO_BOOTSTRAP_H
