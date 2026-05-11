#include "sync/git_sync_backend.h"

#include <git2.h>

#include "utils/logger.h"

namespace vxcore {

GitSyncBackend::GitSyncBackend() = default;

GitSyncBackend::~GitSyncBackend() {
  // Best-effort shutdown to release libgit2 handles.
  Shutdown();
}

VxCoreError GitSyncBackend::SetCredentials(const SyncCredentials &creds) {
  std::lock_guard<std::mutex> lock(op_mutex_);
  credentials_ = creds;
  return VXCORE_OK;
}

VxCoreError GitSyncBackend::Initialize(const std::string &root_folder,
                                       const SyncConfig &config) {
  (void)root_folder;
  (void)config;
  return VXCORE_ERR_NOT_IMPLEMENTED;
}

VxCoreError GitSyncBackend::Shutdown() {
  std::lock_guard<std::mutex> lock(op_mutex_);
  if (rebase_in_progress_ != nullptr) {
    git_rebase_free(rebase_in_progress_);
    rebase_in_progress_ = nullptr;
  }
  if (repo_ != nullptr) {
    git_repository_free(repo_);
    repo_ = nullptr;
  }
  credentials_ = {};
  initialized_ = false;
  return VXCORE_OK;
}

VxCoreError GitSyncBackend::Sync(SyncProgressCallback callback, void *userdata) {
  (void)callback; (void)userdata;
  return VXCORE_ERR_NOT_IMPLEMENTED;
}

VxCoreError GitSyncBackend::Push(SyncProgressCallback callback, void *userdata) {
  (void)callback; (void)userdata;
  return VXCORE_ERR_NOT_IMPLEMENTED;
}

VxCoreError GitSyncBackend::Pull(SyncProgressCallback callback, void *userdata) {
  (void)callback; (void)userdata;
  return VXCORE_ERR_NOT_IMPLEMENTED;
}

VxCoreError GitSyncBackend::GetStatus(std::vector<SyncFileInfo> &out_files) {
  (void)out_files;
  return VXCORE_ERR_NOT_IMPLEMENTED;
}

VxCoreError GitSyncBackend::GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) {
  (void)out_conflicts;
  return VXCORE_ERR_NOT_IMPLEMENTED;
}

VxCoreError GitSyncBackend::ResolveConflict(const std::string &path,
                                            SyncConflictResolution resolution) {
  (void)path; (void)resolution;
  return VXCORE_ERR_NOT_IMPLEMENTED;
}

}  // namespace vxcore
