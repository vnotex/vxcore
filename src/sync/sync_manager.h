#ifndef VXCORE_SYNC_MANAGER_H
#define VXCORE_SYNC_MANAGER_H

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include "sync_backend.h"
#include "sync_types.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class EventManager;
class NotebookManager;

class SyncManager {
 public:
  explicit SyncManager(NotebookManager *notebook_manager);
  ~SyncManager();

  void SetEventManager(EventManager *event_manager);

  VxCoreError EnableSync(const std::string &notebook_id, const SyncConfig &config);

  // C++-only overload: EnableSync that sets credentials BEFORE Initialize().
  // Required for backends whose Initialize() needs auth (e.g., authenticated git clone).
  // Intentionally NOT exposed via the C API in v1; called by the Qt layer or tests.
  VXCORE_API VxCoreError EnableSync(const std::string &notebook_id, const SyncConfig &config,
                                    const SyncCredentials &credentials);

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

  VXCORE_API std::vector<std::string> GetDirtyNotebooks() const;

  VXCORE_API void ClearDirty(const std::string &notebook_id);

 private:
  VxCoreError ValidateNotebook(const std::string &notebook_id);

  // Shared implementation for both EnableSync overloads. When credentials != nullptr,
  // calls backend->SetCredentials(*credentials) BEFORE backend->Initialize(...).
  // On any failure, rolls back configs_/states_/backends_ to their prior state.
  VxCoreError EnableSyncImpl(const std::string &notebook_id, const SyncConfig &config,
                             const SyncCredentials *credentials);

  NotebookManager *notebook_manager_;
  EventManager *event_manager_ = nullptr;
  std::unordered_map<std::string, std::unique_ptr<ISyncBackend>> backends_;
  std::unordered_map<std::string, SyncState> states_;
  std::unordered_map<std::string, SyncConfig> configs_;
  mutable std::mutex dirty_mutex_;
  std::set<std::string> dirty_notebooks_;
  std::vector<uint64_t> event_listener_ids_;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_MANAGER_H
