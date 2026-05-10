#ifndef VXCORE_SYNC_BACKEND_H
#define VXCORE_SYNC_BACKEND_H

#include <string>
#include <vector>

#include "sync_types.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class ISyncBackend {
 public:
  virtual ~ISyncBackend() = default;

  virtual VxCoreError Initialize(const std::string &root_folder,
                                 const SyncConfig &config) = 0;

  // Called by SyncManager BEFORE Initialize/Sync/Push/Pull when credentials are
  // available. Backend caches them. Idempotent. Default no-op for backends that
  // don't need credentials (e.g., MockSyncBackend).
  virtual VxCoreError SetCredentials(const SyncCredentials &creds) {
    (void)creds;
    return VXCORE_OK;
  }

  virtual VxCoreError Shutdown() = 0;

  virtual VxCoreError Sync(SyncProgressCallback callback, void *userdata) = 0;

  virtual VxCoreError Push(SyncProgressCallback callback, void *userdata) = 0;

  virtual VxCoreError Pull(SyncProgressCallback callback, void *userdata) = 0;

  virtual VxCoreError GetStatus(std::vector<SyncFileInfo> &out_files) = 0;

  virtual VxCoreError GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) = 0;

  virtual VxCoreError ResolveConflict(const std::string &path,
                                      SyncConflictResolution resolution) = 0;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_BACKEND_H
