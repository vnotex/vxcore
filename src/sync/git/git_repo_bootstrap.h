#ifndef VXCORE_SYNC_GIT_GIT_REPO_BOOTSTRAP_H
#define VXCORE_SYNC_GIT_GIT_REPO_BOOTSTRAP_H

#include <git2.h>

#include <memory>
#include <string>

#include "sync/sync_types.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class ICredentialProvider;

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
// Wave 6.3 F4.4 of sync-backend-phase4: the credentials parameter has been
// replaced with a shared_ptr<ICredentialProvider>. The pipeline forwards it
// to MakeRemoteCallbacks at each network-touching phase, taking a fresh
// SyncCredentials snapshot per callback build.
VxCoreError OpenExistingRepo(const std::string &root_folder,
                             const std::string &git_dir,
                             const SyncConfig &config,
                             std::shared_ptr<ICredentialProvider> creds_provider,
                             git_rebase **rebase_in_progress,
                             git_repository **out_repo);

// Branch 2 (T15): clone-into-empty-remote.
VxCoreError BootstrapFromEmptyRemote(const std::string &root_folder,
                                     const std::string &git_dir,
                                     const SyncConfig &config,
                                     std::shared_ptr<ICredentialProvider> creds_provider,
                                     git_rebase **rebase_in_progress,
                                     git_repository **out_repo);

// Branch 3 (T16): init+push to empty-remote.
VxCoreError BootstrapToEmptyRemote(const std::string &root_folder,
                                   const std::string &git_dir,
                                   const SyncConfig &config,
                                   std::shared_ptr<ICredentialProvider> creds_provider,
                                   git_rebase **rebase_in_progress,
                                   git_repository **out_repo);

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_REPO_BOOTSTRAP_H
