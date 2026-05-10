#include "sync_manager.h"

#include "core/notebook.h"
#include "core/notebook_manager.h"
#include "utils/logger.h"

namespace vxcore {

SyncManager::SyncManager(NotebookManager *notebook_manager)
    : notebook_manager_(notebook_manager) {
  VXCORE_LOG_INFO("SyncManager initialized");
}

SyncManager::~SyncManager() {
  VXCORE_LOG_INFO("SyncManager shutting down");
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

  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  configs_[notebook_id] = config;
  states_[notebook_id] = SyncState::kIdle;

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

  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) {
    return err;
  }

  if (states_.find(notebook_id) == states_.end()) {
    return VXCORE_ERR_SYNC_NOT_ENABLED;
  }

  if (backends_.find(notebook_id) == backends_.end()) {
    return VXCORE_ERR_NOT_IMPLEMENTED;
  }

  return VXCORE_OK;
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

  if (backends_.find(notebook_id) == backends_.end()) {
    return VXCORE_ERR_NOT_IMPLEMENTED;
  }

  return VXCORE_OK;
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

  if (backends_.find(notebook_id) == backends_.end()) {
    return VXCORE_ERR_NOT_IMPLEMENTED;
  }

  return VXCORE_OK;
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

  if (backends_.find(notebook_id) == backends_.end()) {
    return VXCORE_ERR_NOT_IMPLEMENTED;
  }

  return VXCORE_OK;
}

}  // namespace vxcore
