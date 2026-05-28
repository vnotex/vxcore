#include "sync_manager.h"

#include <mutex>

#include "core/event_manager.h"
#include "core/event_names.h"
#include "core/notebook.h"
#include "core/notebook_manager.h"
#include "core/work_queue.h"
#include "sync/credential_provider.h"
#include "sync/git/libgit2_init.h"
#include "sync/sync_backend_registry.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

SyncManager::SyncManager(NotebookManager *notebook_manager)
    : notebook_manager_(notebook_manager) {
  VXCORE_LOG_DEBUG("SyncManager initialized");
}

SyncManager::~SyncManager() {
  if (event_manager_) {
    for (auto id : event_listener_ids_) {
      event_manager_->Unsubscribe(id);
    }
  }
  VXCORE_LOG_INFO("SyncManager shutting down");
}

void SyncManager::SetEventManager(EventManager *event_manager) {
  event_manager_ = event_manager;
  if (!event_manager_) return;

  auto mark_dirty = [this](const std::string &event_name, const nlohmann::json &data) {
    if (data.contains("notebookId") && data["notebookId"].is_string()) {
      std::string nb_id = data["notebookId"].get<std::string>();
      // Task 7.5 (F3.2): cache-presence is the correct predicate here — it
      // means "EnableSync has populated runtime state for this notebook in
      // this process". Wave 10.1 (F2.4 part 2): the read MUST take
      // state_mutex_ because the event can fire from any thread while
      // EnableSync/DisableSync mutate configs_cache_ on another thread.
      // Lock is released BEFORE invoking DirtyTracker / MaybeEnqueueSync —
      // both of which fan out to external code (work queue + GetSyncConfig
      // which itself re-takes state_mutex_).
      bool sync_enabled;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        sync_enabled = (configs_cache_.count(nb_id) > 0);
      }
      VXCORE_LOG_DEBUG("SyncManager::mark_dirty: event=%s notebookId=%s sync_enabled=%d",
                       event_name.c_str(), nb_id.c_str(), sync_enabled ? 1 : 0);
      if (sync_enabled) {
        // Wave 9.2 (F2.4): delegate to DirtyTracker (self-locking). Empty
        // path placeholder preserves the current per-notebook-only semantics;
        // per-path enrichment is future work.
        dirty_tracker_.MarkDirty(nb_id, "");
        VXCORE_LOG_DEBUG("SyncManager: marked notebook dirty: %s", nb_id.c_str());
        MaybeEnqueueSync(nb_id);
      }
    }
  };

  event_listener_ids_.push_back(event_manager_->Subscribe(events::kFileCreated, mark_dirty));
  event_listener_ids_.push_back(event_manager_->Subscribe(events::kFileSaved, mark_dirty));
  event_listener_ids_.push_back(event_manager_->Subscribe(events::kFileDeleted, mark_dirty));
  event_listener_ids_.push_back(event_manager_->Subscribe(events::kFileMoved, mark_dirty));
  event_listener_ids_.push_back(event_manager_->Subscribe(events::kFolderCreated, mark_dirty));
  event_listener_ids_.push_back(event_manager_->Subscribe(events::kFolderDeleted, mark_dirty));
}

void SyncManager::SetWorkQueueManager(WorkQueueManager *work_queue_manager) {
  work_queue_manager_ = work_queue_manager;
}

void SyncManager::MaybeEnqueueSync(const std::string &notebook_id) {
  // (Debounce removal fix): vxcore no longer applies debounce inside MaybeEnqueueSync.
  // Each call emits sync.should_run exactly once, regardless of interval. The debounce
  // gate that checked "interval_seconds <= 0" and "now - last_enqueue_time_ < interval"
  // has been removed. The Qt-side SyncWorkQueueManager coalesces concurrent triggers via
  // coalesceKey="trigger", which is the correct and only dedup mechanism. Without this
  // fix, the second folder-created event within 60s was silently dropped, leaving the
  // notebook dirty with no timer to wake it up (user-visible bug: second folder never
  // synced).
  //
  // last_enqueue_time_ is preserved (or can be removed entirely if no caller reads it).
  // It now records the timestamp of each sync.should_run emission for potential
  // diagnostic/observability purposes. It is NOT consulted as a gate anywhere.
  //
  // Locking: state_mutex_ is NOT taken here. GetSyncConfig acquires the lock
  // internally; the EventManager::Emit fan-out at the end is external and
  // MUST run outside any SyncManager lock (Wave 0.5 contract).
  VXCORE_LOG_DEBUG("SyncManager::MaybeEnqueueSync: entry notebook_id=%s has_em=%d",
                   notebook_id.c_str(), event_manager_ != nullptr);
  if (!event_manager_) {
    VXCORE_LOG_DEBUG("SyncManager::MaybeEnqueueSync: skipped (missing event_manager)");
    return;
  }

  // Verify notebook has sync config (even though we no longer debounce,
  // we still need a valid SyncConfig to proceed). If config is missing or
  // interval_seconds <= 0, skip emission.
  SyncConfig effective_cfg;
  if (GetSyncConfig(notebook_id, effective_cfg) != VXCORE_OK ||
      effective_cfg.interval_seconds <= 0) {
    VXCORE_LOG_DEBUG("SyncManager::MaybeEnqueueSync: skipped (no config or interval<=0) "
                     "notebook_id=%s",
                     notebook_id.c_str());
    return;
  }

  // Record the timestamp for observability (no longer used as a debounce gate).
  last_enqueue_time_[notebook_id] = std::chrono::steady_clock::now();

  // Emit sync.should_run OUTSIDE any SyncManager lock. EventManager listeners
  // may re-enter SyncManager (e.g., the Qt auto-route consumer that calls TriggerSync).
  event_manager_->Emit(events::kSyncShouldRun, {{"notebookId", notebook_id}});
  VXCORE_LOG_INFO("SyncManager: emitted sync.should_run for notebook: %s", notebook_id.c_str());
}

std::vector<std::string> SyncManager::GetDirtyNotebooks() const {
  // Wave 9.2 (F2.4): delegate to DirtyTracker (self-locking).
  return dirty_tracker_.ListDirtyNotebooks();
}

void SyncManager::ClearDirty(const std::string &notebook_id) {
  // Wave 9.2 (F2.4): delegate to DirtyTracker (self-locking).
  dirty_tracker_.Clear(notebook_id);
}

bool SyncManager::IsReady(const std::string &notebook_id) const {
  // Task 7.4 (F3.6): single source of truth for "sync configured & ready".
  // Replicates the predicate previously inlined in vxcore_sync_is_ready —
  // we deliberately do NOT also require states_.count(notebook_id) so that
  // disk-complete-but-runtime-absent notebooks (S4) still report ready and
  // the reconcile path can lift them into S5. No callbacks fired; no
  // state_mutex_ taken (this reads notebook config only — not the runtime
  // maps).
  auto *notebook = notebook_manager_->GetNotebook(notebook_id);
  if (!notebook) {
    return false;
  }
  const auto &cfg = notebook->GetConfig();
  return cfg.sync_enabled && !cfg.sync_backend.empty() && !cfg.sync_remote_url.empty();
}

int64_t SyncManager::LastSyncTime(const std::string &notebook_id) const {
  // Task 7.4 (F3.6): authoritative per-device last-successful-sync timestamp
  // (ms since Unix epoch, UTC). Delegates to Notebook::GetLastSyncUtc which
  // reads metadata.db — the same value SyncManager::TriggerSync writes via
  // Notebook::SetLastSyncUtc(GetCurrentTimestampMillis()) on VXCORE_OK.
  auto *notebook = notebook_manager_->GetNotebook(notebook_id);
  if (!notebook) {
    return 0;
  }
  return notebook->GetLastSyncUtc();
}

VxCoreError SyncManager::ValidateNotebook(const std::string &notebook_id) {
  auto *notebook = notebook_manager_->GetNotebook(notebook_id);
  if (notebook == nullptr) {
    return VXCORE_ERR_NOT_FOUND;
  }
  if (notebook->GetType() == NotebookType::Raw) {
    return VXCORE_ERR_UNSUPPORTED;
  }
  return VXCORE_OK;
}

VxCoreError SyncManager::EnableSync(const std::string &notebook_id, const SyncConfig &config) {
  VXCORE_LOG_DEBUG("SyncManager::EnableSync: notebook_id=%s", notebook_id.c_str());
  return EnableSyncImpl(notebook_id, config, nullptr, nullptr);
}

VxCoreError SyncManager::EnableSync(const std::string &notebook_id, const SyncConfig &config,
                                    const SyncCredentials &credentials) {
  VXCORE_LOG_DEBUG("SyncManager::EnableSync(creds): notebook_id=%s", notebook_id.c_str());
  // Wave 6.3 F4.4: legacy SyncCredentials overload is now a thin wrapper —
  // creds get wrapped in an InMemoryCredentialProvider and routed through
  // the provider pipeline. The backend never sees the raw SyncCredentials.
  auto provider = std::make_shared<InMemoryCredentialProvider>(credentials);
  return EnableSyncImpl(notebook_id, config, std::move(provider), nullptr);
}

VxCoreError SyncManager::EnableSync(const std::string &notebook_id, const SyncConfig &config,
                                    std::shared_ptr<ICredentialProvider> creds_provider) {
  VXCORE_LOG_DEBUG("SyncManager::EnableSync(provider): notebook_id=%s", notebook_id.c_str());
  return EnableSyncImpl(notebook_id, config, std::move(creds_provider), nullptr);
}

VxCoreError SyncManager::EnableSyncImpl(const std::string &notebook_id, const SyncConfig &config,
                                         std::shared_ptr<ICredentialProvider> provider,
                                         SyncBackendFactory factory_override) {
  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  // Production guards apply only to the registry path. When a factory_override
  // is supplied (test path), the factory IS the source of truth — the
  // libgit2 init guard is bypassed so tests can run on hosts where libgit2 init
  // failed (e.g., the mock-only test executable).
  if (!factory_override) {
    if (config.backend == "git" && !LibGit2Init::ok()) {
      VXCORE_LOG_ERROR("SyncManager::EnableSyncImpl: libgit2 init failed for notebook: %s",
                       notebook_id.c_str());
      return VXCORE_ERR_GIT_INIT_FAILED;
    }
  }

  // Wave 10.1 (F2.4 part 2): seed configs_cache_/states_ under lock. Snapshot
  // prior values so the rollback lambda can restore them on failure. The
  // backend->Initialize() call below is EXTERNAL and MUST run outside the
  // lock (it can call back into SyncManager — see test_sync_manager_reentrancy).
  bool had_config;
  bool had_state;
  SyncConfig prev_config;
  SyncState prev_state = SyncState::kIdle;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    had_config = configs_cache_.count(notebook_id) > 0;
    had_state = states_.count(notebook_id) > 0;
    if (had_config) prev_config = configs_cache_[notebook_id];
    if (had_state) prev_state = states_[notebook_id];

    configs_cache_[notebook_id] = config;
    states_[notebook_id] = SyncState::kIdle;
  }

  auto rollback = [&]() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (had_config) {
      configs_cache_[notebook_id] = prev_config;
    } else {
      configs_cache_.erase(notebook_id);
    }
    if (had_state) {
      states_[notebook_id] = prev_state;
    } else {
      states_.erase(notebook_id);
    }
  };

  // Factory dispatch (Task 6.2 F4.4). Provider is forwarded to both paths.
  // External: registry / supplied factory may construct a backend whose ctor
  // touches libgit2 — keep it outside state_mutex_.
  std::unique_ptr<ISyncBackend> backend;
  if (factory_override) {
    backend = factory_override(config, provider);
  } else {
    const std::string backend_name = config.backend.empty() ? "git" : config.backend;
    backend = SyncBackendRegistry::Instance().Create(backend_name, config, provider);
  }
  if (!backend) {
    VXCORE_LOG_WARN("SyncManager::EnableSyncImpl: factory returned null for backend '%s' "
                    "(notebook: %s)",
                    config.backend.c_str(), notebook_id.c_str());
    rollback();
    return VXCORE_ERR_UNKNOWN_BACKEND;
  }

  // Wave 6.3 F4.4: AuthRequired now demands a non-null provider. The legacy
  // SetCredentials escape hatch is gone — callers that have raw SyncCredentials
  // must wrap them in an InMemoryCredentialProvider (the EnableSync(id, cfg,
  // creds) overload does this transparently). Rolls back maps so the caller
  // can retry without leaving the notebook half-enabled.
  if ((backend->GetCapabilities() &
       static_cast<uint32_t>(SyncCapability::AuthRequired)) &&
      !provider) {
    VXCORE_LOG_WARN("SyncManager::EnableSyncImpl: backend '%s' requires AuthRequired "
                    "but no ICredentialProvider was supplied (notebook: %s)",
                    config.backend.c_str(), notebook_id.c_str());
    backend.reset();
    rollback();
    return VXCORE_ERR_MISSING_CREDENTIALS;
  }

  // Wave 6.3 F4.4: defense-in-depth — even when the factory's ctor took the
  // provider, we also forward it through ReplaceCredsProvider so backends
  // that ignore the ctor arg (e.g., the legacy MockBackendProxy used in
  // tests) still receive the provider. The base-class default is a no-op,
  // so backends that don't need credentials pay no cost.
  // EXTERNAL CALL — runs outside state_mutex_.
  if (provider) {
    backend->ReplaceCredsProvider(provider);
  }

  auto *notebook = notebook_manager_->GetNotebook(notebook_id);
  // EXTERNAL CALL — backend->Initialize may block on network I/O (libgit2
  // clone), fire progress callbacks, or call back into SyncManager.
  // MUST run outside state_mutex_.
  err = backend->Initialize(notebook->GetRootFolder(), config);
  if (err != VXCORE_OK) {
    rollback();
    return err;
  }

  // Wave 10.1: publish the constructed backend under lock so concurrent
  // TriggerSync/GetSyncStatus/etc. see it atomically.
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    backends_[notebook_id] = std::move(backend);
  }
  VXCORE_LOG_INFO("Sync enabled for notebook: %s, backend: %s", notebook_id.c_str(),
                  config.backend.c_str());
  return VXCORE_OK;
}

VxCoreError SyncManager::DisableSync(const std::string &notebook_id) {
  VXCORE_LOG_DEBUG("SyncManager::DisableSync: notebook_id=%s", notebook_id.c_str());

  // Wave 10.1 (F2.4 part 2): move the owned backend out of the map under
  // lock, then destroy it OUTSIDE the lock. The backend dtor is external
  // (libgit2 shutdown, fd close) and could in principle re-enter SyncManager
  // — keep it out of the critical section.
  std::unique_ptr<ISyncBackend> doomed_backend;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = backends_.find(notebook_id);
    if (it != backends_.end()) {
      doomed_backend = std::move(it->second);
      backends_.erase(it);
    }
    states_.erase(notebook_id);
    configs_cache_.erase(notebook_id);
  }
  // doomed_backend destructs here (lock released).

  VXCORE_LOG_INFO("Sync disabled for notebook: %s", notebook_id.c_str());
  return VXCORE_OK;
}

VxCoreError SyncManager::GetSyncConfig(const std::string &notebook_id, SyncConfig &out_config) {
  VXCORE_LOG_DEBUG("SyncManager::GetSyncConfig: notebook_id=%s", notebook_id.c_str());

  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  // Task 7.5 (F3.2): configs_cache_ is a read-through cache; the authoritative
  // store is the per-notebook NotebookConfig JSON. Fast path: cache hit
  // returns immediately. Slow path: re-hydrate from notebook JSON and
  // populate the cache for next time. InvalidateConfigCache(id) (called by
  // external mutators) forces the slow path on the next request.
  // Wave 10.1 (F2.4 part 2): fast-path cache hit served entirely under lock.
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = configs_cache_.find(notebook_id);
    if (it != configs_cache_.end()) {
      out_config = it->second;
      return VXCORE_OK;
    }
  }

  // Slow path: read notebook JSON outside the lock (notebook ptr is stable
  // for the lifetime of the open notebook, and GetConfig is a const accessor
  // on the notebook itself).
  auto *notebook = notebook_manager_->GetNotebook(notebook_id);
  const auto &nc = notebook->GetConfig();
  out_config.backend = nc.sync_backend;
  out_config.remote_url = nc.sync_remote_url;
  out_config.interval_seconds = nc.sync_interval_seconds;

  // Re-acquire lock to populate cache.
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    configs_cache_[notebook_id] = out_config;
  }
  return VXCORE_OK;
}

void SyncManager::InvalidateConfigCache(const std::string &notebook_id) {
  VXCORE_LOG_DEBUG("SyncManager::InvalidateConfigCache: notebook_id=%s", notebook_id.c_str());
  std::lock_guard<std::mutex> lock(state_mutex_);
  configs_cache_.erase(notebook_id);
}

VxCoreError SyncManager::TriggerSync(const std::string &notebook_id) {
  // Wave 12.2 / F5.9: legacy no-token wrapper. Forwards to the cancellable
  // overload with null — preserves pre-W12 behavior bit-for-bit.
  return TriggerSync(notebook_id, nullptr);
}

VxCoreError SyncManager::TriggerSync(const std::string &notebook_id,
                                     SyncCancellationPtr cancellation) {
  VXCORE_LOG_DEBUG("SyncManager::TriggerSync: notebook_id=%s cancellable=%d",
                   notebook_id.c_str(), cancellation ? 1 : 0);

  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  // Wave 10.1 (F2.4 part 2): take a snapshot of the backend raw pointer and
  // perform the SYNC_NOT_ENABLED + NOT_IMPLEMENTED checks under lock, then
  // release before calling backend->Sync() (external — libgit2 network I/O,
  // progress callbacks, re-entry into SyncManager are all permitted).
  // The backend pointer is stable: DisableSync moves the unique_ptr out
  // under the same lock and destroys it after release, so a concurrent
  // DisableSync racing with TriggerSync sees one of two valid orderings.
  // Wave 12.2 (F5.9): in-flight cancellation now works — the caller passes
  // a token, we call backend->SetCancellation(token) BEFORE Sync(), and
  // unconditionally clear it afterwards.
  ISyncBackend *backend_ptr = nullptr;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const size_t states_count = states_.count(notebook_id);
    VXCORE_LOG_DEBUG("SyncManager::TriggerSync: states_ size=%zu count(notebook_id)=%zu "
                     "(0 means reconcile-on-open did not populate runtime state)",
                     states_.size(), states_count);

    if (states_.find(notebook_id) == states_.end()) {
      return VXCORE_ERR_SYNC_NOT_ENABLED;
    }

    auto backend_it = backends_.find(notebook_id);
    if (backend_it == backends_.end()) {
      return VXCORE_ERR_NOT_IMPLEMENTED;
    }
    backend_ptr = backend_it->second.get();
    states_[notebook_id] = SyncState::kStaging;
  }

  // T7 (sync-queue-convergence): emit sync.started OUTSIDE state_mutex_
  // (the lock above was already released by the closing brace). EventManager
  // fan-out is "external" per AGENTS.md § SyncManager Locking Discipline
  // rule 3 — listeners may call back into SyncManager.
  if (event_manager_) {
    event_manager_->Emit(events::kSyncStarted, {{"notebookId", notebook_id}});
  }

  // EXTERNAL CALLS — outside state_mutex_. SetCancellation runs first so
  // an early Cancel() between install and Sync() entry is still observed
  // by the in-flight Sync().
  backend_ptr->SetCancellation(cancellation);
  // Wave 13.1 (F5.7 part 2): wire progress callback through the dispatcher.
  // The lambda captures only progress_dispatcher_ by reference — NO
  // state_mutex_ touch on the callback hot path. The dispatcher self-locks
  // and copies-before-invoke per Wave 0.5 contract.
  SyncProgressCallback progress_cb =
      [this](const SyncProgress &progress, void * /*userdata*/) {
        progress_dispatcher_.Dispatch(progress);
      };
  VxCoreError sync_err = backend_ptr->Sync(progress_cb, nullptr);
  // ALWAYS clear the token to avoid stale state on the next Sync() that
  // arrives without one. Safe even when SetCancellation is a no-op (Mock /
  // future non-cancellable backends).
  backend_ptr->SetCancellation(nullptr);

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (sync_err == VXCORE_OK) {
      states_[notebook_id] = SyncState::kIdle;
    } else if (sync_err == VXCORE_ERR_SYNC_CONFLICT) {
      states_[notebook_id] = SyncState::kConflicted;
    } else {
      states_[notebook_id] = SyncState::kError;
    }
  }

  if (sync_err == VXCORE_OK) {
    // DirtyTracker is self-locking and external to state_mutex_.
    ClearDirty(notebook_id);
    // Persist per-device "last successful sync" timestamp. Best-effort:
    // failure to write is logged inside SetLastSyncUtc but does NOT fail
    // the sync (the sync itself succeeded -- only the UI timestamp suffers).
    auto *notebook = notebook_manager_->GetNotebook(notebook_id);
    if (notebook) {
      notebook->SetLastSyncUtc(GetCurrentTimestampMillis());
    }
  }

  // T8 (sync-queue-convergence): emit sync.conflict BEFORE sync.finished,
  // enriched with the conflict file list. GetConflicts is external and MUST
  // run outside state_mutex_ — the lock above was already released. Backend
  // pointer stability mirrors the Sync() call above.
  if (sync_err == VXCORE_ERR_SYNC_CONFLICT && event_manager_) {
    std::vector<SyncConflictInfo> conflicts;
    backend_ptr->GetConflicts(conflicts);
    nlohmann::json files = nlohmann::json::array();
    for (const auto &c : conflicts) {
      files.push_back(c.path);
    }
    event_manager_->Emit(events::kSyncConflict,
                         {{"notebookId", notebook_id}, {"files", files}});
  }

  // T7 (sync-queue-convergence): emit sync.finished OUTSIDE state_mutex_.
  // Payload carries the result code so subscribers can distinguish
  // OK / CONFLICT / NETWORK / AUTH_FAILED / ... without a separate event.
  if (event_manager_) {
    event_manager_->Emit(events::kSyncFinished,
                         {{"notebookId", notebook_id},
                          {"result", static_cast<int>(sync_err)}});
  }
  return sync_err;
}

VxCoreError SyncManager::GetSyncStatus(const std::string &notebook_id, SyncState &out_state,
                                       std::vector<SyncFileInfo> &out_files) {
  VXCORE_LOG_DEBUG("SyncManager::GetSyncStatus: notebook_id=%s", notebook_id.c_str());

  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  // Wave 10.1 (F2.4 part 2): snapshot state + backend pointer under lock,
  // release before calling backend->GetStatus() (external).
  ISyncBackend *backend_ptr = nullptr;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (states_.find(notebook_id) == states_.end()) {
      return VXCORE_ERR_SYNC_NOT_ENABLED;
    }
    out_state = states_[notebook_id];

    auto backend_it = backends_.find(notebook_id);
    if (backend_it == backends_.end()) {
      out_files.clear();
      return VXCORE_OK;
    }
    backend_ptr = backend_it->second.get();
  }

  return backend_ptr->GetStatus(out_files);
}

VxCoreError SyncManager::GetConflicts(const std::string &notebook_id,
                                      std::vector<SyncConflictInfo> &out_conflicts) {
  VXCORE_LOG_DEBUG("SyncManager::GetConflicts: notebook_id=%s", notebook_id.c_str());

  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  // Wave 10.1 (F2.4 part 2): see GetSyncStatus.
  ISyncBackend *backend_ptr = nullptr;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (states_.find(notebook_id) == states_.end()) {
      return VXCORE_ERR_SYNC_NOT_ENABLED;
    }

    auto backend_it = backends_.find(notebook_id);
    if (backend_it == backends_.end()) {
      out_conflicts.clear();
      return VXCORE_OK;
    }
    backend_ptr = backend_it->second.get();
  }

  return backend_ptr->GetConflicts(out_conflicts);
}

VxCoreError SyncManager::ResolveConflict(const std::string &notebook_id, const std::string &path,
                                         SyncConflictResolution resolution) {
  VXCORE_LOG_DEBUG("SyncManager::ResolveConflict: notebook_id=%s path=%s", notebook_id.c_str(),
                   path.c_str());

  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  // Wave 10.1 (F2.4 part 2): snapshot backend under lock, release before
  // calling external ResolveConflict + GetConflicts. Re-acquire lock to
  // flip states_ entry if the conflict set is now empty.
  ISyncBackend *backend_ptr = nullptr;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (states_.find(notebook_id) == states_.end()) {
      return VXCORE_ERR_SYNC_NOT_ENABLED;
    }
    auto backend_it = backends_.find(notebook_id);
    if (backend_it == backends_.end()) {
      return VXCORE_ERR_NOT_IMPLEMENTED;
    }
    backend_ptr = backend_it->second.get();
  }

  VxCoreError resolve_err = backend_ptr->ResolveConflict(path, resolution);
  if (resolve_err == VXCORE_OK) {
    std::vector<SyncConflictInfo> remaining;
    backend_ptr->GetConflicts(remaining);
    if (remaining.empty()) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      states_[notebook_id] = SyncState::kIdle;
    }
  }
  return resolve_err;
}

VxCoreError SyncManager::UpdateCredentials(const std::string &notebook_id,
                                           std::shared_ptr<ICredentialProvider> provider) {
  VXCORE_LOG_DEBUG("SyncManager::UpdateCredentials: notebook_id=%s", notebook_id.c_str());

  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  // Wave 10.1 (F2.4 part 2): snapshot backend pointer under lock, release
  // before calling backend->ReplaceCredsProvider (external — backend has
  // its own creds_provider_mu_).
  ISyncBackend *backend_ptr = nullptr;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (states_.find(notebook_id) == states_.end()) {
      return VXCORE_ERR_SYNC_NOT_ENABLED;
    }
    auto backend_it = backends_.find(notebook_id);
    if (backend_it == backends_.end()) {
      return VXCORE_ERR_NOT_IMPLEMENTED;
    }
    backend_ptr = backend_it->second.get();
  }

  // Wave 6.3 F4.4: atomic provider swap on the backend. The backend's
  // own creds_provider_mu_ serializes the shared_ptr replacement; any
  // in-flight Sync() uses the snapshot it captured at its own entry point
  // and is unaffected by this rotation.
  backend_ptr->ReplaceCredsProvider(std::move(provider));
  return VXCORE_OK;
}

VxCoreError SyncManager::EnableSyncWithFactoryForTesting(
    const std::string &notebook_id, const SyncConfig &config,
    std::shared_ptr<ICredentialProvider> provider, SyncBackendFactory factory_override) {
  VXCORE_LOG_DEBUG("SyncManager::EnableSyncWithFactoryForTesting: notebook_id=%s",
                   notebook_id.c_str());
  return EnableSyncImpl(notebook_id, config, std::move(provider), std::move(factory_override));
}

// Wave 13.1 (F5.7 part 2): progress dispatcher forwarders.
SyncProgressDispatcher::ObserverId SyncManager::RegisterProgressObserver(
    std::function<void(const SyncProgress &)> observer) {
  return progress_dispatcher_.Register(std::move(observer));
}

void SyncManager::UnregisterProgressObserver(SyncProgressDispatcher::ObserverId id) {
  progress_dispatcher_.Unregister(id);
}

}  // namespace vxcore
