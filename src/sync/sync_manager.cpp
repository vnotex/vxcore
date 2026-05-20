#include "sync_manager.h"

#include "core/event_manager.h"
#include "core/event_names.h"
#include "core/notebook.h"
#include "core/notebook_manager.h"
#include "core/work_queue.h"
#include "sync/git/git_sync_backend.h"
#include "sync/git/libgit2_init.h"
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
      const bool sync_enabled = (configs_.count(nb_id) > 0);
      VXCORE_LOG_DEBUG("SyncManager::mark_dirty: event=%s notebookId=%s sync_enabled=%d",
                       event_name.c_str(), nb_id.c_str(), sync_enabled ? 1 : 0);
      if (sync_enabled) {
        std::lock_guard<std::mutex> lock(dirty_mutex_);
        dirty_notebooks_.insert(nb_id);
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
  // Called under dirty_mutex_.
  VXCORE_LOG_DEBUG("SyncManager::MaybeEnqueueSync: entry notebook_id=%s has_wqm=%d has_em=%d",
                   notebook_id.c_str(), work_queue_manager_ != nullptr,
                   event_manager_ != nullptr);
  if (!work_queue_manager_ || !event_manager_) {
    VXCORE_LOG_DEBUG("SyncManager::MaybeEnqueueSync: skipped (missing wqm or em)");
    return;
  }

  auto cfg_it = configs_.find(notebook_id);
  if (cfg_it == configs_.end() || cfg_it->second.interval_seconds <= 0) {
    VXCORE_LOG_DEBUG("SyncManager::MaybeEnqueueSync: skipped (no config or interval<=0) notebook_id=%s",
                     notebook_id.c_str());
    return;
  }

  auto now = std::chrono::steady_clock::now();
  auto interval = std::chrono::seconds(cfg_it->second.interval_seconds);
  auto &last = last_enqueue_time_[notebook_id];
  if (last != std::chrono::steady_clock::time_point{} && (now - last) < interval) {
    VXCORE_LOG_DEBUG("SyncManager::MaybeEnqueueSync: debounced notebook_id=%s interval_s=%d",
                     notebook_id.c_str(), cfg_it->second.interval_seconds);
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
  std::lock_guard<std::mutex> lock(dirty_mutex_);
  return {dirty_notebooks_.begin(), dirty_notebooks_.end()};
}

void SyncManager::ClearDirty(const std::string &notebook_id) {
  std::lock_guard<std::mutex> lock(dirty_mutex_);
  dirty_notebooks_.erase(notebook_id);
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
  return EnableSyncImpl(notebook_id, config, nullptr);
}

VxCoreError SyncManager::EnableSync(const std::string &notebook_id, const SyncConfig &config,
                                    const SyncCredentials &credentials) {
  VXCORE_LOG_DEBUG("SyncManager::EnableSync(creds): notebook_id=%s", notebook_id.c_str());
  return EnableSyncImpl(notebook_id, config, &credentials);
}

VxCoreError SyncManager::EnableSyncImpl(const std::string &notebook_id, const SyncConfig &config,
                                         const SyncCredentials *credentials) {
  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  // Fail-fast on unknown backend (F1.4/B3) — check BEFORE init to catch config errors early.
  if (!config.backend.empty() && config.backend != "git" && config.backend != "mock") {
    VXCORE_LOG_ERROR("SyncManager::EnableSyncImpl: unknown backend '%s' for notebook: %s",
                     config.backend.c_str(), notebook_id.c_str());
    return VXCORE_ERR_UNKNOWN_BACKEND;
  }

  // Check if libgit2 initialization succeeded (F2.5/B4) — only for git backend.
  if (config.backend == "git" && !LibGit2Init::ok()) {
    VXCORE_LOG_ERROR("SyncManager::EnableSyncImpl: libgit2 init failed for notebook: %s",
                     notebook_id.c_str());
    return VXCORE_ERR_GIT_INIT_FAILED;
  }

  const bool had_config = configs_.count(notebook_id) > 0;
  const bool had_state = states_.count(notebook_id) > 0;
  SyncConfig prev_config;
  SyncState prev_state = SyncState::kIdle;
  if (had_config) prev_config = configs_[notebook_id];
  if (had_state) prev_state = states_[notebook_id];

  configs_[notebook_id] = config;
  states_[notebook_id] = SyncState::kIdle;

  auto rollback = [&]() {
    if (had_config) {
      configs_[notebook_id] = prev_config;
    } else {
      configs_.erase(notebook_id);
    }
    if (had_state) {
      states_[notebook_id] = prev_state;
    } else {
      states_.erase(notebook_id);
    }
  };

  // Factory: build a backend if config.backend matches a known type.
  std::unique_ptr<ISyncBackend> backend;
  if (config.backend == "git") {
    backend = std::make_unique<GitSyncBackend>();
  }
  // Unknown backend: leave config + state stored, no backend registered.
  if (!backend) {
    VXCORE_LOG_INFO("Sync enabled for notebook: %s, backend: %s (no factory)",
                    notebook_id.c_str(), config.backend.c_str());
    return VXCORE_OK;
  }

  if (credentials != nullptr) {
    err = backend->SetCredentials(*credentials);
    if (err != VXCORE_OK) {
      rollback();
      return err;
    }
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
  configs_.erase(notebook_id);

  VXCORE_LOG_INFO("Sync disabled for notebook: %s", notebook_id.c_str());
  return VXCORE_OK;
}

VxCoreError SyncManager::GetSyncConfig(const std::string &notebook_id, SyncConfig &out_config) {
  VXCORE_LOG_DEBUG("SyncManager::GetSyncConfig: notebook_id=%s", notebook_id.c_str());

  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  auto it = configs_.find(notebook_id);
  if (it != configs_.end()) {
    out_config = it->second;
    return VXCORE_OK;
  }

  auto *notebook = notebook_manager_->GetNotebook(notebook_id);
  const auto &nc = notebook->GetConfig();
  out_config.enabled = nc.sync_enabled;
  out_config.backend = nc.sync_backend;
  out_config.remote_url = nc.sync_remote_url;
  out_config.interval_seconds = nc.sync_interval_seconds;
  return VXCORE_OK;
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

VxCoreError SyncManager::SetCredentials(const std::string &notebook_id,
                                        const SyncCredentials &credentials) {
  VXCORE_LOG_DEBUG("SyncManager::SetCredentials: notebook_id=%s", notebook_id.c_str());

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

  return backend_it->second->SetCredentials(credentials);
}

void SyncManager::RegisterBackendForTesting(const std::string &notebook_id,
                                            std::unique_ptr<ISyncBackend> backend) {
  states_[notebook_id] = SyncState::kIdle;
  backends_[notebook_id] = std::move(backend);
}

VxCoreError SyncManager::EnableSyncWithBackendForTesting(
    const std::string &notebook_id, const SyncConfig &config,
    const SyncCredentials *credentials, std::unique_ptr<ISyncBackend> backend) {
  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  const bool had_config = configs_.count(notebook_id) > 0;
  const bool had_state = states_.count(notebook_id) > 0;
  SyncConfig prev_config;
  SyncState prev_state = SyncState::kIdle;
  if (had_config) prev_config = configs_[notebook_id];
  if (had_state) prev_state = states_[notebook_id];

  configs_[notebook_id] = config;
  states_[notebook_id] = SyncState::kIdle;

  if (credentials != nullptr) {
    err = backend->SetCredentials(*credentials);
    if (err != VXCORE_OK) {
      if (had_config) {
        configs_[notebook_id] = prev_config;
      } else {
        configs_.erase(notebook_id);
      }
      if (had_state) {
        states_[notebook_id] = prev_state;
      } else {
        states_.erase(notebook_id);
      }
      return err;
    }
  }

  auto *notebook = notebook_manager_->GetNotebook(notebook_id);
  err = backend->Initialize(notebook->GetRootFolder(), config);
  if (err != VXCORE_OK) {
    if (had_config) {
      configs_[notebook_id] = prev_config;
    } else {
      configs_.erase(notebook_id);
    }
    if (had_state) {
      states_[notebook_id] = prev_state;
    } else {
      states_.erase(notebook_id);
    }
    return err;
  }

  backends_[notebook_id] = std::move(backend);
  return VXCORE_OK;
}

}  // namespace vxcore
