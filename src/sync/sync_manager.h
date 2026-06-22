#ifndef VXCORE_SYNC_MANAGER_H
#define VXCORE_SYNC_MANAGER_H

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "credential_provider.h"
#include "dirty_tracker.h"
#include "sync_backend.h"
#include "sync_backend_registry.h"
#include "sync_cancellation.h"
#include "sync_progress_dispatcher.h"
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
  // mutating any internal map (configs_cache_/states_/backends_) so the caller can
  // retry safely. Intentionally NOT exposed via the C ABI in v1.
  VXCORE_API VxCoreError EnableSync(const std::string &notebook_id, const SyncConfig &config,
                                    std::shared_ptr<ICredentialProvider> creds_provider);

  VXCORE_API VxCoreError DisableSync(const std::string &notebook_id);

  // Release the in-memory backend for one notebook (frees libgit2 repo
  // handle via ~GitSyncBackend, which on Windows unmaps mmapped pack
  // files and closes their file descriptors).  Does NOT touch on-disk
  // JSON sync_* fields, does NOT touch the OS keychain, does NOT fire
  // any sync-disable events.  After this returns, the runtime state
  // (backends_ + states_ + configs_cache_) is cleared for this
  // notebook but on-disk sync_* fields remain set.  Whether a later
  // EnableSync succeeds without further user input depends on whether
  // the consumer kept credentials available — vxcore does NOT manage
  // credential lifecycle.
  //
  // Use this on notebook close to drop runtime sync state without
  // tearing down the user's persisted sync configuration.
  //
  // Idempotent: returns VXCORE_OK on a notebook that was never
  // registered.
  VXCORE_API VxCoreError UnregisterBackend(const std::string &notebook_id);

  // T18 of open-notebook-remote-readonly plan.
  //
  // Backend-agnostic clone orchestrator. Dispatches to the registered
  // backend by name (config.backend) via SyncBackendRegistry, performs the
  // clone into target_dir, validates the result IS a VNote notebook, and
  // registers it via NotebookManager.
  //
  // Preconditions: target_dir exists, is empty, and is writable. Caller
  // is responsible for creating + verifying the directory.
  //
  // Postconditions:
  //   On VXCORE_OK: out_notebook_id is set; the notebook is registered
  //                 with NotebookManager (subsequent vxcore_notebook_list
  //                 will include it); the .git layout exists at
  //                 <target_dir>/vx_notebook/vx_sync/; sync is NOT
  //                 registered in states_ -- caller must call EnableSync
  //                 separately if they want sync registered.
  //   On failure:  target_dir contents are UNDEFINED (may have partial
  //                clone state). Caller MUST clean up target_dir.
  //                NO notebook is registered with NotebookManager.
  //
  // Error codes (passes through Clone errors plus):
  //   VXCORE_ERR_UNKNOWN_BACKEND -- config.backend not registered
  //   VXCORE_ERR_INVALID_PARAM   -- config.backend empty
  //   VXCORE_ERR_NOT_IMPLEMENTED -- backend doesn't advertise Cloneable
  //   VXCORE_ERR_NOT_FOUND       -- clone succeeded but vx_notebook/config.json missing
  //   VXCORE_ERR_JSON_PARSE      -- vx_notebook/config.json malformed
  //   VXCORE_ERR_INVALID_STATE   -- config.json valid JSON but missing/empty "id"
  VXCORE_API VxCoreError CloneNotebook(const std::string &target_dir,
                                       const SyncConfig &config,
                                       std::shared_ptr<ICredentialProvider> provider,
                                       std::string &out_notebook_id);

  // Cancellable overload. Mirrors the legacy CloneNotebook above and adds
  // an optional SyncCancellationPtr that is installed on the transient
  // backend BEFORE the Clone() call and cleared AFTER, regardless of
  // result. Lifecycle matches TriggerSync(id, cancellation). Null token
  // degrades to the legacy non-cancellable path bit-for-bit. The 4-arg
  // overload above forwards here with nullptr.
  //
  // Note: no state_mutex_ involvement. The backend is constructed locally
  // and never registered in backends_; only the cancellation token is
  // installed on it for the lifetime of the Clone call.
  VXCORE_API VxCoreError CloneNotebook(const std::string &target_dir,
                                       const SyncConfig &config,
                                       std::shared_ptr<ICredentialProvider> provider,
                                       SyncCancellationPtr cancellation,
                                       std::string &out_notebook_id);

  VXCORE_API VxCoreError TriggerSync(const std::string &notebook_id);

  // Wave 12.2 / F5.9: cancellable overload. Threads @token through to the
  // backend via ISyncBackend::SetCancellation BEFORE the Sync() call and
  // clears it (SetCancellation(nullptr)) afterwards regardless of result.
  // A null token means "no cancellation wired" — semantically identical to
  // the legacy TriggerSync(id) overload (which now forwards here with
  // nullptr). Backends that ignore SetCancellation (default no-op) see no
  // behavioural change.
  VXCORE_API VxCoreError TriggerSync(const std::string &notebook_id,
                                     SyncCancellationPtr cancellation);

  // vxcore-sync-stage-only V1: run only the working-tree-touching phase
  // (StageAll + CommitIndex). Does NOT contact the remote. The consumer
  // typically holds a per-notebook lock around this call and releases it
  // before invoking NetworkPhaseOnly so concurrent saves on the same
  // notebook resume immediately after the local commit lands.
  //
  // Cancellation: the token (if non-null) is installed on the backend
  // before the phase runs and cleared afterwards regardless of result —
  // mirrors TriggerSync(id, cancellation) semantics. Backends that ignore
  // SetCancellation see no behavioural change.
  //
  // @param out_did_commit (optional) set to true if a commit was created,
  //   false when the index matched HEAD (nothing to stage). May be nullptr.
  // @return VXCORE_ERR_SYNC_NOT_ENABLED / VXCORE_ERR_NOT_IMPLEMENTED in
  //   the same situations as TriggerSync; VXCORE_ERR_NOT_IMPLEMENTED also
  //   when the registered backend does not override StageAndCommit
  //   (e.g., legacy mocks).
  VXCORE_API VxCoreError StageOnly(const std::string &notebook_id,
                                   SyncCancellationPtr cancellation,
                                   bool *out_did_commit);

  // vxcore-sync-stage-only V1: run only the network phase (FetchOrigin +
  // RebaseOntoOrigin + PushOrigin, including the existing retry policy).
  // Caller MUST have invoked StageOnly (or equivalently TriggerSync) prior
  // so local commits exist to push. Safe to call WITHOUT holding the
  // consumer's per-notebook lock.
  VXCORE_API VxCoreError NetworkPhaseOnly(const std::string &notebook_id,
                                          SyncCancellationPtr cancellation);

  VXCORE_API VxCoreError GetSyncStatus(const std::string &notebook_id, SyncState &out_state,
                            std::vector<SyncFileInfo> &out_files);

  VXCORE_API VxCoreError GetConflicts(const std::string &notebook_id,
                           std::vector<SyncConflictInfo> &out_conflicts);

  VXCORE_API VxCoreError ResolveConflict(const std::string &notebook_id, const std::string &path,
                              SyncConflictResolution resolution);

  VXCORE_API VxCoreError GetSyncConfig(const std::string &notebook_id, SyncConfig &out_config);

  // Task 7.5 (F3.2): invalidate the in-memory SyncConfig cache entry for one
  // notebook. Callers MUST invoke this whenever the persisted NotebookConfig
  // sync_* fields may have changed under us (e.g., notebook close/reload,
  // direct vxcore_notebook_update_config from outside SyncManager). The next
  // GetSyncConfig call will then re-read from notebook JSON and repopulate.
  // No callbacks fired; no state_/backends_ touched. Safe to call for
  // notebooks that have never been cached (no-op). NOT exposed via C ABI.
  VXCORE_API void InvalidateConfigCache(const std::string &notebook_id);

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

  // Task 7.4 (F3.6): authoritative "is sync ready" predicate. Mirrors the
  // semantics formerly inlined in vxcore_sync_is_ready: returns true iff the
  // notebook exists AND its persisted config has sync_enabled=true AND
  // sync_backend non-empty AND sync_remote_url non-empty. The runtime
  // states_/backends_ registration is NOT required — a notebook in S4 (disk
  // complete, runtime absent) still reports ready so the Qt-side reconcile
  // path can pick it up. Const + no callbacks fired.
  VXCORE_API bool IsReady(const std::string &notebook_id) const;

  // Lightweight runtime registration check. Returns true if the notebook is
  // present in states_ (i.e., sync was enabled successfully and the backend
  // is alive). Does NOT touch the backend — pure metadata query under
  // state_mutex_ only. Safe to call from UI threads at high frequency
  // without contending with worker-thread sync operations that hold the
  // per-backend op_mutex_. Use in preference to GetSyncStatus when the
  // caller only needs the "registered?" predicate (e.g. SyncStateClassifier).
  // Wave 14: introduced to fix the GUI-vs-worker op_mutex_ race that produced
  // persistent VXCORE_ERR_SYNC_IN_PROGRESS on every Sync Now click.
  VXCORE_API bool IsRegistered(const std::string &notebook_id) const;

  // Task 7.4 (F3.6): authoritative "last successful sync timestamp" accessor
  // (milliseconds since Unix epoch, UTC). Reads the per-device value from the
  // notebook's metadata.db via Notebook::GetLastSyncUtc. Returns 0 when the
  // notebook is unknown or never synced on this device. Const + no callbacks.
  VXCORE_API int64_t LastSyncTime(const std::string &notebook_id) const;

  // Persist the per-device "last successful sync timestamp" (milliseconds since
  // Unix epoch, UTC) into the notebook's metadata.db via
  // Notebook::SetLastSyncUtc. Mirrors LastSyncTime (the getter). No-op when the
  // notebook is unknown. Lock-free like the getter and NOT a backend/external
  // call; callers MUST serialize metadata.db access (in VNote this runs on the
  // GUI thread from SyncService::onSyncFinished, matching all other
  // metadata.db access — see VNote root AGENTS.md Save Path Threading Contract).
  VXCORE_API void SetLastSyncTime(const std::string &notebook_id, int64_t utc_millis);

  // Wave 13.1 (F5.7 part 2): register a progress observer that will be invoked
  // OUTSIDE state_mutex_ for every progress event produced by an in-flight
  // backend Sync(). The observer is invoked on whatever thread the backend
  // produces the callback on (today: the thread that called TriggerSync —
  // usually a sync worker thread). Cross-thread consumers (Qt SyncService)
  // MUST bounce to their target thread via Qt::QueuedConnection inside the
  // observer lambda.
  //
  // Returns a non-zero ObserverId on success; pass it to
  // UnregisterProgressObserver to remove. Returns 0 if observer is empty.
  VXCORE_API SyncProgressDispatcher::ObserverId
  RegisterProgressObserver(std::function<void(const SyncProgress &)> observer);

  // Wave 13.1 (F5.7 part 2): remove a previously-registered progress observer.
  // No-op if id is 0 or unknown.
  VXCORE_API void UnregisterProgressObserver(SyncProgressDispatcher::ObserverId id);

  // T7 (sync-queue-convergence): read-only accessor for tests that need to
  // reach a registered backend (e.g., to flip a MockSyncBackend return code).
  // NOT thread-safe; caller must serialize against EnableSync/DisableSync.
  // NEVER exposed on the C ABI.
  std::unordered_map<std::string, std::unique_ptr<ISyncBackend>> &BackendsForTesting() {
    return backends_;
  }

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
  // On any failure rolls back configs_cache_/states_/backends_ to their prior
  // state. NEVER exposed on the C ABI.
  VxCoreError EnableSyncImpl(const std::string &notebook_id, const SyncConfig &config,
                             std::shared_ptr<ICredentialProvider> provider,
                             SyncBackendFactory factory_override);

  // Emit the "sync.should_run" event for a sync-enabled notebook whose
  // auto_sync_enabled gate is true. vxcore performs NO debounce/throttle — the
  // consumer owns all scheduling policy (the previous "sync" WorkQueue enqueue
  // path was removed). last_enqueue_time_ is recorded for observability only.
  // Wave 9.2 (F2.4): no longer holds dirty_mutex_ (DirtyTracker is self-locking
  // and was already released by the caller before invocation).
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
  // Task 7.5 (F3.2): SyncConfig cache. Authoritative store is the per-notebook
  // NotebookConfig sync_* fields on disk (written by the Qt SyncService
  // layer); this map is a read-through cache populated lazily by
  // GetSyncConfig and eagerly by EnableSyncImpl. Mutable so const accessors
  // (and future const overloads) can refresh it without const_cast. Always
  // mutated under the same caller-supplied serialization that wraps every
  // other SyncManager method — Wave 10 introduces state_mutex_ to make this
  // explicit.
  mutable std::unordered_map<std::string, SyncConfig> configs_cache_;
  // Wave 9.2 (F2.4): dirty queue delegated to DirtyTracker (self-locking).
  // Bridging: SyncManager's current public API is per-notebook only — we pass
  // empty path "" to MarkDirty so HasDirty/Clear semantics are preserved.
  // Per-path enrichment is future work.
  DirtyTracker dirty_tracker_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_enqueue_time_;
  std::vector<uint64_t> event_listener_ids_;

  // Wave 10.1 (F2.4 part 2): coarse mutex guarding the three runtime maps —
  // configs_cache_, states_, backends_. Single mutex (no per-member locks,
  // no recursive_mutex). Wave 0.5 contract: NEVER hold this lock across any
  // external invocation (hook, signal, libgit2 progress callback, credential
  // provider call, work-queue dispatch, backend method call). Standard
  // pattern: copy needed state into stack locals under the lock, release
  // (via scoped block), then invoke the external code. See
  // "SyncManager Locking Discipline" in libs/vxcore/src/sync/AGENTS.md.
  // mutable: const accessors (IsReady, GetDirtyNotebooks — though GetDirty
  // currently uses DirtyTracker only) and the read-through cache path in
  // GetSyncConfig need to take it.
  mutable std::mutex state_mutex_;

  // Wave 13.1 (F5.7 part 2): progress fan-out. Self-locking; NEVER held with
  // state_mutex_. The lambda passed to backend->Sync() forwards into
  // progress_dispatcher_.Dispatch() directly — no SyncManager locks taken
  // on the callback hot path.
  SyncProgressDispatcher progress_dispatcher_;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_MANAGER_H
