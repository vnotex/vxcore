#ifndef VXCORE_SYNC_GIT_GIT_SYNC_PIPELINE_H
#define VXCORE_SYNC_GIT_GIT_SYNC_PIPELINE_H

#include <git2.h>

#include <memory>
#include <string>

#include "sync/git/git_credential_callback.h"
#include "sync/sync_types.h"

namespace vxcore {

class ICredentialProvider;

// Single-shot pipeline of git sync phases. Stateless across calls except
// for the rebase-in-progress handle borrowed from the caller (GitSyncBackend).
// Composition root (GitSyncBackend) owns the retry loop; pipeline provides
// the individual phase methods.
//
// Wave 6.3 (F4.4) of sync-backend-phase4: the pipeline no longer caches a
// SyncCredentials reference. Instead it holds a shared_ptr<ICredentialProvider>
// taken by the backend via GetCredsProviderSnapshot() at the entry of each
// Sync()/Initialize()/ResolveConflict() call. This gives per-Sync()
// atomicity: a concurrent ReplaceCredsProvider() on the backend does NOT
// affect an in-flight pipeline (the in-flight uses the snapshot it captured;
// the NEXT Sync() picks up the new provider). Each phase that touches the
// network calls MakeRemoteCallbacks(provider, url) which takes a FRESH
// SyncCredentials snapshot at callback-build time. The commit-author info
// (user.name / user.email) is snapshotted ONCE at pipeline construction so
// all commits made through the same pipeline use a stable signature.
class GitSyncPipeline {
 public:
  GitSyncPipeline(git_repository *repo, const std::string &git_dir, const std::string &root_folder,
                  const SyncConfig &config,
                  std::shared_ptr<ICredentialProvider> creds_provider,
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
  // shared_ptr keeps the provider alive for the pipeline's lifetime —
  // immune to concurrent backend-level ReplaceCredsProvider() swaps.
  std::shared_ptr<ICredentialProvider> creds_provider_;
  // Snapshotted at ctor: stable commit-author identity for the lifetime of
  // this pipeline. Defaults to empty (-> built-in VNote defaults) when no
  // provider is supplied or when the provider declines.
  std::string author_name_;
  std::string author_email_;
  git_rebase **rebase_in_progress_;  // borrowed pointer to backend's field
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_SYNC_PIPELINE_H
