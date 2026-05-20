#ifndef VXCORE_SYNC_GIT_GIT_SYNC_BACKEND_H
#define VXCORE_SYNC_GIT_GIT_SYNC_BACKEND_H

#include "sync/sync_backend.h"
#include "sync/git/libgit2_init.h"
#include "vxcore/vxcore_types.h"

#include <memory>
#include <mutex>
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
class GitSyncBackend : public ISyncBackend {
 public:
  VXCORE_API GitSyncBackend();
  // Constructor accepting a SyncConfig — exists to match the
  // SyncBackendFactory signature used by SyncBackendRegistry (Task 4.2 of
  // sync-backend-phase4). The cfg parameter is intentionally ignored here;
  // real configuration arrives via Initialize(root_folder, config).
  VXCORE_API explicit GitSyncBackend(const SyncConfig &cfg);
  VXCORE_API ~GitSyncBackend() override;

  GitSyncBackend(const GitSyncBackend&) = delete;
  GitSyncBackend& operator=(const GitSyncBackend&) = delete;
  GitSyncBackend(GitSyncBackend&&) = delete;
  GitSyncBackend& operator=(GitSyncBackend&&) = delete;

  // ISyncBackend
  std::string GetName() const override;
  SyncCapabilities GetCapabilities() const override;
  bool IsInitialized() const override;
  VxCoreError SetCredentials(const SyncCredentials &creds) override;
  VxCoreError Initialize(const std::string &root_folder,
                         const SyncConfig &config) override;
  VxCoreError Shutdown() override;
  VxCoreError Sync(SyncProgressCallback callback, void *userdata) override;
  VxCoreError Push(SyncProgressCallback callback, void *userdata) override;
  VxCoreError Pull(SyncProgressCallback callback, void *userdata) override;
  VxCoreError GetStatus(std::vector<SyncFileInfo> &out_files) override;
  VxCoreError GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) override;
  VxCoreError ResolveConflict(const std::string &path,
                              SyncConflictResolution resolution) override;

  // Construct a GitSyncPipeline wired to this backend's internal repo and
  // config. The returned pipeline borrows references to backend state, so
  // the pipeline MUST NOT outlive this GitSyncBackend instance. Holding the
  // returned unique_ptr beyond the backend's lifetime is undefined behavior.
  //
  // Returns nullptr if the backend has not been initialized. op_mutex_ is
  // held only during factory construction and released before return — the
  // caller is responsible for serializing pipeline use against concurrent
  // Initialize() / Shutdown() on the same backend (single-threaded callers
  // need no extra synchronization).
  //
  // Exposed primarily for tests that drive Sync sub-phases (Stage / Commit /
  // Fetch / Rebase / Push) in isolation. Production code constructs pipelines
  // inline within Initialize / Sync / Push / Pull.
  VXCORE_API std::unique_ptr<GitSyncPipeline> MakePipeline();

 private:
  LibGit2Init libgit2_;     // RAII; FIRST member so it's last destroyed
  std::mutex op_mutex_;     // serializes per-backend libgit2 calls
  std::string root_folder_; // notebook root (= libgit2 workdir)
  std::string git_dir_;     // root_folder_ + "/vx_notebook/vx_sync/"
  SyncConfig config_;
  SyncCredentials credentials_;
  git_repository *repo_ = nullptr;
  git_rebase *rebase_in_progress_ = nullptr; // T24 sets when rebase pauses on conflict
  bool initialized_ = false;
};

// Anchor function used to defeat dead-stripping of git_sync_backend.cpp by
// MSVC's /OPT:REF. The .cpp file installs a BackendRegistration token in an
// anonymous namespace; nothing in vxcore.dll references that token directly
// once Task 4.4 lands. Calling EnsureGitBackendLinked() from another TU (we
// call it from LibGit2Init's ctor) forces the linker to keep the entire .obj
// alive, which keeps the static-init registration alive too. The function
// body itself is a tiny no-op that touches the registration symbol.
VXCORE_API void EnsureGitBackendLinked();

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_SYNC_BACKEND_H
