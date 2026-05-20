#ifndef VXCORE_SYNC_MANAGER_H
#define VXCORE_SYNC_MANAGER_H

#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include "credential_provider.h"
#include "sync_backend.h"
#include "sync_backend_registry.h"
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

  // Task 6.2 (F4.4) C++-only overload: EnableSync with an explicit
  // ICredentialProvider. The provider is forwarded to the backend factory.
  // Backends declaring SyncCapability::AuthRequired MUST receive either a
  // non-null provider here OR credentials via the (id, cfg, creds) overload
  // — passing neither returns VXCORE_ERR_MISSING_CREDENTIALS WITHOUT
  // mutating any internal map (configs_/states_/backends_) so the caller can
  // retry safely. Intentionally NOT exposed via the C ABI in v1.
  VXCORE_API VxCoreError EnableSync(const std::string &notebook_id, const SyncConfig &config,
                                    std::shared_ptr<ICredentialProvider> creds_provider);

  VXCORE_API VxCoreError DisableSync(const std::string &notebook_id);

  VXCORE_API VxCoreError TriggerSync(const std::string &notebook_id);

  VXCORE_API VxCoreError GetSyncStatus(const std::string &notebook_id, SyncState &out_state,
                            std::vector<SyncFileInfo> &out_files);

  VXCORE_API VxCoreError GetConflicts(const std::string &notebook_id,
                           std::vector<SyncConflictInfo> &out_conflicts);

  VXCORE_API VxCoreError ResolveConflict(const std::string &notebook_id, const std::string &path,
                              SyncConflictResolution resolution);

  VXCORE_API VxCoreError GetSyncConfig(const std::string &notebook_id, SyncConfig &out_config);

  // C++-only: rotate credentials on an already-enabled notebook by swapping
  // the backend's ICredentialProvider. Wave 6.3 (F4.4) replaces the legacy
  // SetCredentials path — callers wrap a SyncCredentials in an
  // InMemoryCredentialProvider and pass it here.
  //
  // Returns:
  //   VXCORE_ERR_NOT_FOUND if notebook unknown
  //   VXCORE_ERR_SYNC_NOT_ENABLED if state absent
  //   VXCORE_ERR_NOT_IMPLEMENTED if no backend registered
  //   VXCORE_OK on success
  // Intentionally NOT exposed via vxcore.h C API in v1 (the C ABI's
  // vxcore_sync_set_credentials wraps a SyncCredentials and calls this).
  VXCORE_API VxCoreError UpdateCredentials(const std::string &notebook_id,
                                           std::shared_ptr<ICredentialProvider> provider);

  // Task 5.1 (F4.2): test-only EnableSync that injects a backend FACTORY instead
  // of a fully-constructed backend instance. Wave 6.3 F4.4 reshaped the
  // signature: SyncCredentials* parameter replaced with
  // shared_ptr<ICredentialProvider>. Tests that need to inject creds wrap
  // them in an InMemoryCredentialProvider.
  //
  // Intentionally NOT exposed on the C ABI (vxcore.h).
  VXCORE_API VxCoreError EnableSyncWithFactoryForTesting(const std::string &notebook_id,
                                                         const SyncConfig &config,
                                                         std::shared_ptr<ICredentialProvider> provider,
                                                         SyncBackendFactory factory_override);

  VXCORE_API std::vector<std::string> GetDirtyNotebooks() const;

  VXCORE_API void ClearDirty(const std::string &notebook_id);

 private:
  VxCoreError ValidateNotebook(const std::string &notebook_id);

  // Shared implementation for EnableSync overloads. Wave 6.3 F4.4 collapsed
  // the legacy SyncCredentials* parameter — callers that have a
  // SyncCredentials wrap it in an InMemoryCredentialProvider before calling
  // here. When provider is non-null and the backend declares
  // SyncCapability::AuthRequired, the provider is the sole credential source
  // (no separate SetCredentials hop).
  //
  // When factory_override is non-null it is used to construct the backend
  // (test path, registry guards bypassed). Otherwise
  // SyncBackendRegistry::Instance().Create(...) runs with the full production
  // guards (libgit2 init check for "git", unknown-backend rejection).
  //
  // On any failure rolls back configs_/states_/backends_ to their prior
  // state. NEVER exposed on the C ABI.
  VxCoreError EnableSyncImpl(const std::string &notebook_id, const SyncConfig &config,
                             std::shared_ptr<ICredentialProvider> provider,
                             SyncBackendFactory factory_override);

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
