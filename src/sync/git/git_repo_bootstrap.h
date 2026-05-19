#ifndef VXCORE_SYNC_GIT_GIT_REPO_BOOTSTRAP_H
#define VXCORE_SYNC_GIT_GIT_REPO_BOOTSTRAP_H

#include <git2.h>

#include <string>

#include "sync/sync_types.h"
#include "vxcore/vxcore_types.h"

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
void ScrubGitlink(const std::string &root_folder, const std::string &context);

// Branch 1 (T14): re-open an existing libgit2 repo at |git_dir|.
//
// Preconditions: |git_dir| exists, contains HEAD (caller has verified the
// corrupt-repo guard). The caller has just called RebindCoreWorktree to fix
// up any stale core.worktree before this is invoked.
//
// Behavior:
//   1. git_repository_open(git_dir)
//   2. git_repository_set_workdir(repo, root_folder, update_gitlink=0)
//   3. git_remote_lookup("origin") — missing origin returns INVALID_PARAM
//   4. Verify origin URL matches config.remote_url — mismatch returns
//      INVALID_PARAM
//   5. GitSyncPipeline::ApplyDefaultGitConfig() — enforces filemode, autocrlf,
//      gpgsign, author
//
// On success: *out_repo holds the opened repo (caller owns; must
// git_repository_free on Shutdown).
// On failure: *out_repo is nullptr; any partially-acquired libgit2 handles
// have been freed inside.
VxCoreError OpenExistingRepo(const std::string &root_folder,
                             const std::string &git_dir,
                             const SyncConfig &config,
                             const SyncCredentials &credentials,
                             git_rebase **rebase_in_progress,
                             git_repository **out_repo);

// Branch 2 (T15): clone-into-empty-remote. The notebook root has no user
// files (only vx_notebook/ at most) and a remote URL is configured.
//
// Preconditions: !PathExists(git_dir), notebook root has no user files
// (excluding vx_notebook/), config.remote_url is non-empty.
//
// Behavior: init a libgit2 repo with the separate gitdir layout
// (vx_notebook/vx_sync/), attach origin, fetch with credentials, then
// checkout the remote's default branch (main, fall back to master).
// git_clone is intentionally avoided — its Windows local-transport path
// hangs on file:// URLs.
//
// If the remote is truly empty (no refs at all), we return VXCORE_OK with
// the local repo left as-is (HEAD symbolic to refs/heads/main); future
// Sync() will create the first commit.
//
// On success: *out_repo holds the opened repo + ScrubGitlink has been
// invoked.
// On failure: *out_repo is nullptr; any partially-created handles freed.
VxCoreError BootstrapFromEmptyRemote(const std::string &root_folder,
                                     const std::string &git_dir,
                                     const SyncConfig &config,
                                     const SyncCredentials &credentials,
                                     git_rebase **rebase_in_progress,
                                     git_repository **out_repo);

// Branch 3 (T16): init+push to empty-remote. The notebook root has user
// files, the configured remote is empty (caller has verified RemoteHasRefs
// returned false), and a remote URL is set.
//
// Preconditions: !PathExists(git_dir), notebook root has user files,
// config.remote_url non-empty, remote has no refs.
//
// Behavior: init a libgit2 repo with the separate-gitdir layout, write
// .gitignore + .gitattributes defaults, create origin, stage + commit
// everything, push refs/heads/main to publish the notebook.
//
// On success: *out_repo holds the opened repo + ScrubGitlink has been
// invoked.
// On failure: *out_repo is nullptr; any partially-created handles freed.
VxCoreError BootstrapToEmptyRemote(const std::string &root_folder,
                                   const std::string &git_dir,
                                   const SyncConfig &config,
                                   const SyncCredentials &credentials,
                                   git_rebase **rebase_in_progress,
                                   git_repository **out_repo);

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_REPO_BOOTSTRAP_H
