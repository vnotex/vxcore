#ifndef VXCORE_SYNC_GIT_GIT_SYNC_PIPELINE_H
#define VXCORE_SYNC_GIT_GIT_SYNC_PIPELINE_H

#include <git2.h>

#include <memory>
#include <string>

#include "sync/git/git_credential_callback.h"
#include "sync/sync_cancellation.h"
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
  // out_did_commit (optional): set to true when an actual commit object was
  // created, false when the index tree matched HEAD (no-op "nothing to
  // commit" case). Existing callers may pass nullptr and ignore the flag —
  // signature change is source-compatible.
  VxCoreError CommitIndex(const std::string &message, bool *out_did_commit = nullptr);
  VxCoreError FetchOrigin();
  VxCoreError RebaseOntoOrigin();
  VxCoreError PushOrigin();
  VxCoreError ContinueRebaseAfterResolution();

  // W12.1: install (or clear) the cancellation token that libgit2 progress
  // callbacks will consult during network phases. Pass `nullptr` to disable
  // cancellation -- this is the no-op default and preserves pre-W12 behavior.
  // Safe to call between phases on the orchestrator thread; NOT safe to call
  // concurrently with an in-flight phase on this pipeline (the token's
  // atomic semantics let the libgit2 thread observe Cancel() set on any
  // other thread, but swapping the pointer mid-flight races with the
  // bundle that already captured the previous pointer).
  void SetCancellation(SyncCancellationPtr token);

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
  // W12.1: optional cancellation token. Null by default; SetCancellation()
  // installs a token before the orchestrator runs a phase. Held as a
  // shared_ptr so concurrent ownership with the calling SyncManager is
  // explicit -- the libgit2 thread receives only a raw pointer through
  // GitCredentialPayload.cancellation.
  SyncCancellationPtr cancellation_;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_SYNC_PIPELINE_H
