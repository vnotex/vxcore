#ifndef VXCORE_SYNC_GIT_GIT_SYNC_PIPELINE_H
#define VXCORE_SYNC_GIT_GIT_SYNC_PIPELINE_H

#include <git2.h>

#include <string>

#include "sync/git/git_credential_callback.h"
#include "sync/sync_types.h"

namespace vxcore {

// Single-shot pipeline of git sync phases. Stateless across calls except
// for the rebase-in-progress handle borrowed from the caller (GitSyncBackend).
// Composition root (GitSyncBackend) owns the retry loop; pipeline provides
// the individual phase methods.
class GitSyncPipeline {
 public:
  GitSyncPipeline(git_repository *repo, const std::string &git_dir, const std::string &root_folder,
                  const SyncConfig &config, const SyncCredentials &credentials,
                  git_rebase **rebase_in_progress);

  // Phases — each returns VxCoreError, no progress callbacks (Sync orchestrator
  // emits progress via the existing path; phases don't).
  VxCoreError ApplyDefaultGitConfig();
  bool RemoteHasRefs();
  VxCoreError StageAll();
  VxCoreError CommitIndex(const std::string &message);
  VxCoreError FetchOrigin();
  VxCoreError RebaseOntoOrigin();
  VxCoreError PushOrigin();
  VxCoreError ContinueRebaseAfterResolution();

 private:
  git_repository *repo_;
  const std::string &git_dir_;
  const std::string &root_folder_;
  const SyncConfig &config_;
  const SyncCredentials &credentials_;
  git_rebase **rebase_in_progress_;  // borrowed pointer to backend's field
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_SYNC_PIPELINE_H
