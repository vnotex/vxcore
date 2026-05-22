#include "sync_manager.h"

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
  VXCORE_LOG_INFO("SyncManager initialized");
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
      // this process". S4 notebooks (disk complete, runtime absent) are
      // intentionally NOT dirty-tracked until reconcile-on-open lifts them
      // into S5 via EnableSync, which repopulates the cache. The cache is
      // a runtime-presence marker, not a config-disagreement source.
      const bool sync_enabled = (configs_cache_.count(nb_id) > 0);
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
  // Wave 9.2 (F2.4): no longer holds dirty_mutex_ — DirtyTracker is
  // self-locking and the caller in SetEventManager no longer takes a lock.
  VXCORE_LOG_DEBUG("SyncManager::MaybeEnqueueSync: entry notebook_id=%s has_wqm=%d has_em=%d",
                   notebook_id.c_str(), work_queue_manager_ != nullptr,
                   event_manager_ != nullptr);
  if (!work_queue_manager_ || !event_manager_) {
    VXCORE_LOG_DEBUG("SyncManager::MaybeEnqueueSync: skipped (missing wqm or em)");
    return;
  }

  // Task 7.5 (F3.2): pull interval through the cache-aware accessor so any
  // legitimate consumer (cache hit OR JSON-backed S5 notebook whose cache
  // was previously populated) gets a consistent value. mark_dirty already
  // gated entry on cache presence, so the GetSyncConfig fast path is the
  // norm here.
  SyncConfig effective_cfg;
  if (GetSyncConfig(notebook_id, effective_cfg) != VXCORE_OK ||
      effective_cfg.interval_seconds <= 0) {
    VXCORE_LOG_DEBUG("SyncManager::MaybeEnqueueSync: skipped (no config or interval<=0) "
                     "notebook_id=%s",
                     notebook_id.c_str());
    return;
  }

  auto now = std::chrono::steady_clock::now();
  auto interval = std::chrono::seconds(effective_cfg.interval_seconds);
  auto &last = last_enqueue_time_[notebook_id];
  if (last != std::chrono::steady_clock::time_point{} && (now - last) < interval) {
    VXCORE_LOG_DEBUG("SyncManager::MaybeEnqueueSync: debounced notebook_id=%s interval_s=%d",
                     notebook_id.c_str(), effective_cfg.interval_seconds);
    return;
  }

  last = now;

  auto *queue = work_queue_manager_->GetOrCreate("sync");
  std::string nb_id = notebook_id;
  queue->Enqueue([this, nb_id]() {
    event_manager_->Emit(events::kSyncStarted, {{"notebookId", nb_id}});
    VxCoreError err = TriggerSync(nb_id);
    if (err == VXCORE_ERR_SYNC_CONFLICT) {
      event_manager_->Emit(events::kSyncConflict, {{"notebookId", nb_id}});
    }
    event_manager_->Emit(events::kSyncFinished,
                         {{"notebookId", nb_id}, {"result", static_cast<int>(err)}});
  });
  VXCORE_LOG_DEBUG("SyncManager: enqueued auto-sync for notebook: %s", notebook_id.c_str());
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
  // the reconcile path can lift them into S5. No callbacks fired; no lock
  // taken (notebook config is read-only here and current SyncManager methods
  // are caller-synchronized — Wave 10 introduces state_mutex_).
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

  const bool had_config = configs_cache_.count(notebook_id) > 0;
  const bool had_state = states_.count(notebook_id) > 0;
  SyncConfig prev_config;
  SyncState prev_state = SyncState::kIdle;
  if (had_config) prev_config = configs_cache_[notebook_id];
  if (had_state) prev_state = states_[notebook_id];

  configs_cache_[notebook_id] = config;
  states_[notebook_id] = SyncState::kIdle;

  auto rollback = [&]() {
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
  if (provider) {
    backend->ReplaceCredsProvider(provider);
  }

  auto *notebook = notebook_manager_->GetNotebook(notebook_id);
  err = backend->Initialize(notebook->GetRootFolder(), config);
  if (err != VXCORE_OK) {
    rollback();
    return err;
  }

  backends_[notebook_id] = std::move(backend);
  VXCORE_LOG_INFO("Sync enabled for notebook: %s, backend: %s", notebook_id.c_str(),
                  config.backend.c_str());
  return VXCORE_OK;
}

VxCoreError SyncManager::DisableSync(const std::string &notebook_id) {
  VXCORE_LOG_DEBUG("SyncManager::DisableSync: notebook_id=%s", notebook_id.c_str());

  backends_.erase(notebook_id);
  states_.erase(notebook_id);
  configs_cache_.erase(notebook_id);

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
  auto it = configs_cache_.find(notebook_id);
  if (it != configs_cache_.end()) {
    out_config = it->second;
    return VXCORE_OK;
  }

  auto *notebook = notebook_manager_->GetNotebook(notebook_id);
  const auto &nc = notebook->GetConfig();
  out_config.backend = nc.sync_backend;
  out_config.remote_url = nc.sync_remote_url;
  out_config.interval_seconds = nc.sync_interval_seconds;
  configs_cache_[notebook_id] = out_config;
  return VXCORE_OK;
}

void SyncManager::InvalidateConfigCache(const std::string &notebook_id) {
  VXCORE_LOG_DEBUG("SyncManager::InvalidateConfigCache: notebook_id=%s", notebook_id.c_str());
  configs_cache_.erase(notebook_id);
}

VxCoreError SyncManager::TriggerSync(const std::string &notebook_id) {
  VXCORE_LOG_DEBUG("SyncManager::TriggerSync: notebook_id=%s", notebook_id.c_str());
  const size_t states_count = states_.count(notebook_id);
  VXCORE_LOG_DEBUG("SyncManager::TriggerSync: states_ size=%zu count(notebook_id)=%zu "
                   "(0 means reconcile-on-open did not populate runtime state)",
                   states_.size(), states_count);

  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  if (states_.find(notebook_id) == states_.end()) {
    return VXCORE_ERR_SYNC_NOT_ENABLED;
  }

  auto backend_it = backends_.find(notebook_id);
  if (backend_it == backends_.end()) {
    return VXCORE_ERR_NOT_IMPLEMENTED;
  }

  states_[notebook_id] = SyncState::kStaging;
  VxCoreError sync_err = backend_it->second->Sync(nullptr, nullptr);
  if (sync_err == VXCORE_OK) {
    states_[notebook_id] = SyncState::kIdle;
    ClearDirty(notebook_id);
    // Persist per-device "last successful sync" timestamp. Best-effort:
    // failure to write is logged inside SetLastSyncUtc but does NOT fail
    // the sync (the sync itself succeeded -- only the UI timestamp suffers).
    auto *notebook = notebook_manager_->GetNotebook(notebook_id);
    if (notebook) {
      notebook->SetLastSyncUtc(GetCurrentTimestampMillis());
    }
  } else if (sync_err == VXCORE_ERR_SYNC_CONFLICT) {
    states_[notebook_id] = SyncState::kConflicted;
  } else {
    states_[notebook_id] = SyncState::kError;
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

  if (states_.find(notebook_id) == states_.end()) {
    return VXCORE_ERR_SYNC_NOT_ENABLED;
  }

  out_state = states_[notebook_id];

  auto backend_it = backends_.find(notebook_id);
  if (backend_it == backends_.end()) {
    out_files.clear();
    return VXCORE_OK;
  }

  return backend_it->second->GetStatus(out_files);
}

VxCoreError SyncManager::GetConflicts(const std::string &notebook_id,
                                      std::vector<SyncConflictInfo> &out_conflicts) {
  VXCORE_LOG_DEBUG("SyncManager::GetConflicts: notebook_id=%s", notebook_id.c_str());

  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  if (states_.find(notebook_id) == states_.end()) {
    return VXCORE_ERR_SYNC_NOT_ENABLED;
  }

  auto backend_it = backends_.find(notebook_id);
  if (backend_it == backends_.end()) {
    out_conflicts.clear();
    return VXCORE_OK;
  }

  return backend_it->second->GetConflicts(out_conflicts);
}

VxCoreError SyncManager::ResolveConflict(const std::string &notebook_id, const std::string &path,
                                         SyncConflictResolution resolution) {
  VXCORE_LOG_DEBUG("SyncManager::ResolveConflict: notebook_id=%s path=%s", notebook_id.c_str(),
                   path.c_str());

  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  if (states_.find(notebook_id) == states_.end()) {
    return VXCORE_ERR_SYNC_NOT_ENABLED;
  }

  auto backend_it = backends_.find(notebook_id);
  if (backend_it == backends_.end()) {
    return VXCORE_ERR_NOT_IMPLEMENTED;
  }

  VxCoreError resolve_err = backend_it->second->ResolveConflict(path, resolution);
  if (resolve_err == VXCORE_OK) {
    std::vector<SyncConflictInfo> remaining;
    backend_it->second->GetConflicts(remaining);
    if (remaining.empty()) {
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

  if (states_.find(notebook_id) == states_.end()) {
    return VXCORE_ERR_SYNC_NOT_ENABLED;
  }

  auto backend_it = backends_.find(notebook_id);
  if (backend_it == backends_.end()) {
    return VXCORE_ERR_NOT_IMPLEMENTED;
  }

  // Wave 6.3 F4.4: atomic provider swap on the backend. The backend's
  // own creds_provider_mu_ serializes the shared_ptr replacement; any
  // in-flight Sync() uses the snapshot it captured at its own entry point
  // and is unaffected by this rotation.
  backend_it->second->ReplaceCredsProvider(std::move(provider));
  return VXCORE_OK;
}

VxCoreError SyncManager::EnableSyncWithFactoryForTesting(
    const std::string &notebook_id, const SyncConfig &config,
    std::shared_ptr<ICredentialProvider> provider, SyncBackendFactory factory_override) {
  VXCORE_LOG_DEBUG("SyncManager::EnableSyncWithFactoryForTesting: notebook_id=%s",
                   notebook_id.c_str());
  return EnableSyncImpl(notebook_id, config, std::move(provider), std::move(factory_override));
}

}  // namespace vxcore
