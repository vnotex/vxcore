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

  // C++-only: forwards credentials to the registered backend (rotation path).
  // Returns:
  //   VXCORE_ERR_NOT_FOUND if notebook unknown
  //   VXCORE_ERR_SYNC_NOT_ENABLED if state absent
  //   VXCORE_ERR_NOT_IMPLEMENTED if no backend registered
  //   otherwise whatever backend->SetCredentials returns
  // Intentionally NOT exposed via vxcore.h C API in v1.
  VXCORE_API VxCoreError SetCredentials(const std::string &notebook_id,
                                        const SyncCredentials &credentials);

  // Task 5.1 (F4.2): test-only EnableSync that injects a backend FACTORY instead
  // of a fully-constructed backend instance. This exercises the same factory
  // dispatch the production path uses, just with the registry bypassed. When
  // factory_override is non-null it is used to construct the backend; the
  // unknown-backend allow-list and libgit2 ok() guards are skipped because the
  // factory is the source of truth in this path. When factory_override is null,
  // behavior is identical to the public EnableSync(id, cfg) overload.
  //
  // Intentionally NOT exposed on the C ABI (vxcore.h). Public-overload callers
  // always reach the registry path via EnableSync(id, cfg) which delegates to
  // EnableSyncImpl(id, cfg, nullptr, nullptr).
  VXCORE_API VxCoreError EnableSyncWithFactoryForTesting(const std::string &notebook_id,
                                                         const SyncConfig &config,
                                                         const SyncCredentials *credentials,
                                                         SyncBackendFactory factory_override);

  VXCORE_API std::vector<std::string> GetDirtyNotebooks() const;

  VXCORE_API void ClearDirty(const std::string &notebook_id);

 private:
  VxCoreError ValidateNotebook(const std::string &notebook_id);

  // Shared implementation for both EnableSync overloads. When credentials != nullptr,
  // calls backend->SetCredentials(*credentials) BEFORE backend->Initialize(...).
  // On any failure, rolls back configs_/states_/backends_ to their prior state.
  // Delegates to the 5-arg overload with provider = nullptr and
  // factory_override = nullptr (registry path).
  VxCoreError EnableSyncImpl(const std::string &notebook_id, const SyncConfig &config,
                             const SyncCredentials *credentials);

  // Task 6.2 (F4.4): private overload that allows injecting a credential
  // provider and/or backend factory directly. The provider is forwarded to
  // the registry/factory and the resulting backend's GetCapabilities() is
  // checked for SyncCapability::AuthRequired — if set, the call requires
  // either a non-null provider OR a non-null credentials snapshot (the
  // legacy SetCredentials path remains valid until Wave 6.3 rewires the
  // libgit2 credential callback). When both are null, construction is
  // aborted with VXCORE_ERR_MISSING_CREDENTIALS and NO maps are mutated.
  // When factory_override is non-null it is used to construct the backend;
  // otherwise SyncBackendRegistry::Instance().Create(...) runs with the full
  // production guards. NEVER exposed on the C ABI.
  VxCoreError EnableSyncImpl(const std::string &notebook_id, const SyncConfig &config,
                             const SyncCredentials *credentials,
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
