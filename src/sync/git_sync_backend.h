#ifndef VXCORE_GIT_SYNC_BACKEND_H
#define VXCORE_GIT_SYNC_BACKEND_H

#include "sync/sync_backend.h"
#include "sync/libgit2_init.h"
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

 private:
  // T27 credential callback friend; static C-style libgit2 callback needs to read credentials_.
  friend int GitSyncBackendCredentialCb(struct git_credential **out, const char *url,
                                        const char *username_from_url,
                                        unsigned int allowed_types, void *payload);

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

#endif  // VXCORE_GIT_SYNC_BACKEND_H
