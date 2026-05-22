#ifndef VXCORE_SYNC_GIT_GIT_SYNC_BACKEND_H
#define VXCORE_SYNC_GIT_GIT_SYNC_BACKEND_H

#include "sync/credential_provider.h"
#include "sync/retry_policy.h"
#include "sync/sync_backend.h"
#include "sync/git/git_options.h"
#include "sync/git/libgit2_init.h"
#include "vxcore/vxcore_types.h"

#include <memory>
#include <mutex>
#include <random>
#include <string>

// Forward declarations to keep libgit2 types out of the public header.
typedef struct git_repository git_repository;
struct git_rebase;

namespace vxcore {

class GitSyncPipeline;

// Concrete ISyncBackend implementation backed by libgit2.
//
// Repository layout: <root_folder>/vx_notebook/vx_sync/ is the .git directory
// (separate gitdir), the notebook root is the working tree.
// All libgit2 operations are serialized by op_mutex_; concurrent Sync() returns
// VXCORE_ERR_SYNC_IN_PROGRESS via try_lock.
//
// Wave 6.3 F4.4 of sync-backend-phase4: credentials are no longer cached in
// the backend. ReplaceCredsProvider() / GetCredsProviderSnapshot() swap and
// snapshot a shared_ptr<ICredentialProvider> under creds_provider_mu_.
// Sync()/Initialize()/ResolveConflict() take a snapshot at entry and pass
// it into the constructed GitSyncPipeline — providing per-call atomicity
// even if ReplaceCredsProvider fires mid-Sync.
class GitSyncBackend : public ISyncBackend {
 public:
  VXCORE_API GitSyncBackend();
  // Constructor accepting a SyncConfig + ICredentialProvider — matches the
  // SyncBackendFactory signature used by SyncBackendRegistry (Task 6.2 of
  // sync-backend-phase4). The cfg parameter is intentionally ignored here;
  // real configuration arrives via Initialize(root_folder, config). The
  // provider is stored under creds_provider_mu_ and consumed by the libgit2
  // credential callback via MakeRemoteCallbacks at each network phase.
  VXCORE_API GitSyncBackend(const SyncConfig &cfg,
                            std::shared_ptr<ICredentialProvider> creds_provider);
  VXCORE_API ~GitSyncBackend() override;

  GitSyncBackend(const GitSyncBackend&) = delete;
  GitSyncBackend& operator=(const GitSyncBackend&) = delete;
  GitSyncBackend(GitSyncBackend&&) = delete;
  GitSyncBackend& operator=(GitSyncBackend&&) = delete;

  // ISyncBackend
  std::string GetName() const override;
  SyncCapabilities GetCapabilities() const override;
  bool IsInitialized() const override;
  void ReplaceCredsProvider(std::shared_ptr<ICredentialProvider> provider) override;
  std::shared_ptr<ICredentialProvider> GetCredsProviderSnapshot() const override;
  VxCoreError Initialize(const std::string &root_folder,
                         const SyncConfig &config) override;
  VxCoreError Sync(SyncProgressCallback callback, void *userdata) override;
  VxCoreError GetStatus(std::vector<SyncFileInfo> &out_files) override;
  VxCoreError GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) override;
  VxCoreError ResolveConflict(const std::string &path,
                              SyncConflictResolution resolution) override;

  // Construct a GitSyncPipeline wired to this backend's internal repo and
  // config. The returned pipeline borrows the repo/git_dir/config references
  // and holds its own shared_ptr to the current credential provider snapshot,
  // so the pipeline MUST NOT outlive this GitSyncBackend instance.
  //
  // Exposed primarily for tests that drive Sync sub-phases in isolation.
  VXCORE_API std::unique_ptr<GitSyncPipeline> MakePipeline();

 private:
  LibGit2Init libgit2_;     // RAII; FIRST member so it's last destroyed
  std::mutex op_mutex_;     // serializes per-backend libgit2 calls
  std::string root_folder_; // notebook root (= libgit2 workdir)
  std::string git_dir_;     // root_folder_ + "/vx_notebook/vx_sync/"
  SyncConfig config_;
  // Typed view over config_.backend_options, parsed once in Initialize().
  GitOptions options_;
  // Wave 6.3 F4.4: credential provider — sole source of credentials. Mutex
  // protects shared_ptr swaps. The libgit2 credential callback NEVER holds
  // this mutex (callback receives a stack-local payload built by
  // MakeRemoteCallbacks at the caller's site, before any libgit2 thread
  // enters the picture).
  mutable std::mutex creds_provider_mu_;
  std::shared_ptr<ICredentialProvider> creds_provider_;
  git_repository *repo_ = nullptr;
  git_rebase *rebase_in_progress_ = nullptr;
  bool initialized_ = false;

  // F5.10 / Task 11.1 of sync-backend-phase4: retry policy + RNG for the
  // Sync() push-retry loop. RNG seeded once at construction from
  // std::random_device — keeps jitter unpredictable across instances while
  // remaining cheap (no per-call random_device hit). Tests substitute a
  // seeded RNG by calling compute_delay() directly, bypassing this member.
  RetryPolicy retry_policy_{};
  mutable std::mt19937 retry_rng_{std::random_device{}()};
};

// Anchor function used to defeat dead-stripping of git_sync_backend.cpp by
// MSVC's /OPT:REF.
VXCORE_API void EnsureGitBackendLinked();

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_SYNC_BACKEND_H
