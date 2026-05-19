#ifndef VXCORE_SYNC_GIT_GIT_SYNC_BACKEND_H
#define VXCORE_SYNC_GIT_GIT_SYNC_BACKEND_H

#include "sync/sync_backend.h"
#include "sync/git/libgit2_init.h"
#include "vxcore/vxcore_types.h"

#include <mutex>
#include <string>

// Forward declarations to keep libgit2 types out of the public header.
typedef struct git_repository git_repository;
struct git_rebase;

namespace vxcore {

// Concrete ISyncBackend implementation backed by libgit2.
//
// Repository layout: <root_folder>/vx_notebook/vx_sync/ is the .git directory
// (separate gitdir), the notebook root is the working tree.
// All libgit2 operations are serialized by op_mutex_; concurrent Sync() returns
// VXCORE_ERR_SYNC_IN_PROGRESS via try_lock.
class GitSyncBackend : public ISyncBackend {
 public:
  VXCORE_API GitSyncBackend();
  VXCORE_API ~GitSyncBackend() override;

  GitSyncBackend(const GitSyncBackend&) = delete;
  GitSyncBackend& operator=(const GitSyncBackend&) = delete;
  GitSyncBackend(GitSyncBackend&&) = delete;
  GitSyncBackend& operator=(GitSyncBackend&&) = delete;

  // ISyncBackend
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

  // Test-only helper: writes default .gitignore + .gitattributes (using
  // config.exclude_paths) into <dir>, preserving any pre-existing files.
  // Returns count of files actually written.
  // Internal callers (T15/T18) use the same helpers via Initialize.
  static VXCORE_API int WriteDefaultIgnoreAndAttributesForTesting(const std::string &dir,
                                                                  const SyncConfig &config);

  // T28 test hook: exposes the file-scope TranslateGitError translator so
  // unit tests can verify libgit2 error class -> VxCoreError mapping without
  // reaching into anonymous-namespace internals.
  static VXCORE_API VxCoreError TranslateGitErrorForTesting(int git_rc);

  // T21/T22/T23 test hooks: each acquires op_mutex_ and delegates to the
  // private helper. They allow unit tests to drive the Sync sub-steps in
  // isolation without exercising the full Sync() orchestration (which lands
  // in T24+T25+T26).
  VXCORE_API VxCoreError StageAllForTesting();
  VXCORE_API VxCoreError CommitIndexForTesting(const std::string &message);
  VXCORE_API VxCoreError FetchOriginForTesting();

  // T24/T25 test hooks: drive the rebase / push helpers in isolation. Each
  // acquires op_mutex_, checks initialized_, then delegates to the private
  // helper.
  VXCORE_API VxCoreError RebaseOntoOriginForTesting();
  VXCORE_API VxCoreError PushOriginForTesting();

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

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_SYNC_BACKEND_H
