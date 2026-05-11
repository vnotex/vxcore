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

  // C++-only: forwards credentials to the registered backend (rotation path).
  // Returns:
  //   VXCORE_ERR_NOT_FOUND if notebook unknown
  //   VXCORE_ERR_SYNC_NOT_ENABLED if state absent
  //   VXCORE_ERR_NOT_IMPLEMENTED if no backend registered
  //   otherwise whatever backend->SetCredentials returns
  // Intentionally NOT exposed via vxcore.h C API in v1.
  VXCORE_API VxCoreError SetCredentials(const std::string &notebook_id,
                                        const SyncCredentials &credentials);

  // Test-only: bypass the factory and inject any backend instance for the given notebook.
  VXCORE_API void RegisterBackendForTesting(const std::string &notebook_id,
                                            std::unique_ptr<ISyncBackend> backend);

  // Test-only: full EnableSync flow with a supplied backend (skips factory).
  // Calls SetCredentials(creds) BEFORE Initialize(...) when credentials != nullptr.
  VXCORE_API VxCoreError EnableSyncWithBackendForTesting(const std::string &notebook_id,
                                                         const SyncConfig &config,
                                                         const SyncCredentials *credentials,
                                                         std::unique_ptr<ISyncBackend> backend);

 private:
  VxCoreError ValidateNotebook(const std::string &notebook_id);

  NotebookManager *notebook_manager_;
  std::unordered_map<std::string, std::unique_ptr<ISyncBackend>> backends_;
  std::unordered_map<std::string, SyncState> states_;
  std::unordered_map<std::string, SyncConfig> configs_;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_MANAGER_H
