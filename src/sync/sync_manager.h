#ifndef VXCORE_SYNC_MANAGER_H
#define VXCORE_SYNC_MANAGER_H

#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include "sync_backend.h"
#include "sync_types.h"
#include "sync/git/libgit2_init.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class EventManager;
class NotebookManager;
class WorkQueueManager;

class SyncManager {
 public:
  explicit SyncManager(NotebookManager *notebook_manager);
  ~SyncManager();

  VXCORE_API void SetEventManager(EventManager *event_manager);

  VXCORE_API void SetWorkQueueManager(WorkQueueManager *work_queue_manager);

  VXCORE_API VxCoreError EnableSync(const std::string &notebook_id, const SyncConfig &config);

  // C++-only overload: EnableSync that sets credentials BEFORE Initialize().
  // Required for backends whose Initialize() needs auth (e.g., authenticated git clone).
  // Intentionally NOT exposed via the C API in v1; called by the Qt layer or tests.
  VXCORE_API VxCoreError EnableSync(const std::string &notebook_id, const SyncConfig &config,
                                    const SyncCredentials &credentials);

  VXCORE_API VxCoreError DisableSync(const std::string &notebook_id);

  VXCORE_API VxCoreError TriggerSync(const std::string &notebook_id);

  VXCORE_API VxCoreError GetSyncStatus(const std::string &notebook_id, SyncState &out_state,
                            std::vector<SyncFileInfo> &out_files);

  VXCORE_API VxCoreError GetConflicts(const std::string &notebook_id,
                           std::vector<SyncConflictInfo> &out_conflicts);

  VXCORE_API VxCoreError ResolveConflict(const std::string &notebook_id, const std::string &path,
                              SyncConflictResolution resolution);

  VXCORE_API VxCoreError GetSyncConfig(const std::string &notebook_id, SyncConfig &out_config);

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

  // Enqueue a sync job on the "sync" WorkQueue if the debounce interval has elapsed.
  // Called under dirty_mutex_.
  void MaybeEnqueueSync(const std::string &notebook_id);

  // Declared first so libgit2 is initialized before any backend is
  // constructed and shut down after all backends are destroyed (B4/F2.5
  // — ensures LibGit2Init::ok() returns true by the time EnableSync
  // runs, without requiring every caller/test to construct its own
  // guard).
  LibGit2Init libgit2_guard_;
  NotebookManager *notebook_manager_;
  EventManager *event_manager_ = nullptr;
  WorkQueueManager *work_queue_manager_ = nullptr;
  std::unordered_map<std::string, std::unique_ptr<ISyncBackend>> backends_;
  std::unordered_map<std::string, SyncState> states_;
  std::unordered_map<std::string, SyncConfig> configs_;
  mutable std::mutex dirty_mutex_;
  std::set<std::string> dirty_notebooks_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_enqueue_time_;
  std::vector<uint64_t> event_listener_ids_;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_MANAGER_H
