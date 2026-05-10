#ifndef VXCORE_SYNC_MANAGER_H
#define VXCORE_SYNC_MANAGER_H

#include <memory>
#include <string>
#include <unordered_map>

#include "sync_backend.h"
#include "sync_types.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class NotebookManager;

class SyncManager {
 public:
  explicit SyncManager(NotebookManager *notebook_manager);
  ~SyncManager();

  VxCoreError EnableSync(const std::string &notebook_id, const SyncConfig &config);

  VxCoreError DisableSync(const std::string &notebook_id);

  VxCoreError TriggerSync(const std::string &notebook_id);

  VxCoreError GetSyncStatus(const std::string &notebook_id, SyncState &out_state,
                            std::vector<SyncFileInfo> &out_files);

  VxCoreError GetConflicts(const std::string &notebook_id,
                           std::vector<SyncConflictInfo> &out_conflicts);

  VxCoreError ResolveConflict(const std::string &notebook_id, const std::string &path,
                              SyncConflictResolution resolution);

  VxCoreError GetSyncConfig(const std::string &notebook_id, SyncConfig &out_config);

 private:
  VxCoreError ValidateNotebook(const std::string &notebook_id);

  NotebookManager *notebook_manager_;
  std::unordered_map<std::string, std::unique_ptr<ISyncBackend>> backends_;
  std::unordered_map<std::string, SyncState> states_;
  std::unordered_map<std::string, SyncConfig> configs_;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_MANAGER_H
