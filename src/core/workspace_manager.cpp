#include "workspace_manager.h"

#include <algorithm>

#include "config_manager.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

WorkspaceManager::WorkspaceManager(ConfigManager *config_manager)
    : config_manager_(config_manager) {
  LoadWorkspaces();
}

WorkspaceManager::~WorkspaceManager() { SaveWorkspaces(); }

void WorkspaceManager::LoadWorkspaces() {
  auto &session_config = config_manager_->GetSessionConfig();
  workspaces_.clear();

  for (const auto &record : session_config.workspaces) {
    auto workspace = std::make_unique<WorkspaceConfig>();
    workspace->id = record.id;
    workspace->name = record.name;
    workspace->visible = true;  // Default visible
    workspaces_[workspace->id] = std::move(workspace);
    VXCORE_LOG_DEBUG("Loaded workspace: id=%s, name=%s", record.id.c_str(), record.name.c_str());
  }

  current_workspace_id_ = session_config.current_workspace_id;
  VXCORE_LOG_INFO("Loaded %zu workspaces, current=%s", workspaces_.size(),
                  current_workspace_id_.c_str());
}

void WorkspaceManager::SaveWorkspaces() {
  auto &session_config = config_manager_->GetSessionConfig();
  session_config.workspaces.clear();

  for (const auto &pair : workspaces_) {
    WorkspaceRecord record;
    record.id = pair.second->id;
    record.name = pair.second->name;
    session_config.workspaces.push_back(record);
  }

  session_config.current_workspace_id = current_workspace_id_;

  config_manager_->SaveSessionConfig();
  VXCORE_LOG_DEBUG("Saved %zu workspaces to session", workspaces_.size());
}

std::string WorkspaceManager::CreateWorkspace(const std::string &name) {
  auto workspace = std::make_unique<WorkspaceConfig>();
  workspace->id = GenerateUUID();
  workspace->name = name;
  workspace->visible = true;
  workspace->metadata = nlohmann::json::object();

  std::string id = workspace->id;
  workspaces_[id] = std::move(workspace);

  SaveWorkspaces();
  VXCORE_LOG_INFO("Created workspace: id=%s, name=%s", id.c_str(), name.c_str());
  return id;
}

bool WorkspaceManager::DeleteWorkspace(const std::string &id) {
  auto it = workspaces_.find(id);
  if (it == workspaces_.end()) {
    VXCORE_LOG_WARN("Cannot delete non-existent workspace: id=%s", id.c_str());
    return false;
  }

  workspaces_.erase(it);

  // If deleting current workspace, switch to another
  if (current_workspace_id_ == id) {
    if (!workspaces_.empty()) {
      current_workspace_id_ = workspaces_.begin()->first;
    } else {
      current_workspace_id_.clear();
    }
  }

  SaveWorkspaces();
  VXCORE_LOG_INFO("Deleted workspace: id=%s", id.c_str());
  return true;
}

WorkspaceConfig *WorkspaceManager::GetWorkspace(const std::string &id) {
  auto it = workspaces_.find(id);
  if (it == workspaces_.end()) {
    return nullptr;
  }
  return it->second.get();
}

std::vector<WorkspaceConfig> WorkspaceManager::ListWorkspaces() {
  std::vector<WorkspaceConfig> result;
  result.reserve(workspaces_.size());

  for (const auto &pair : workspaces_) {
    result.push_back(*pair.second);
  }

  return result;
}

bool WorkspaceManager::RenameWorkspace(const std::string &id, const std::string &name) {
  auto it = workspaces_.find(id);
  if (it == workspaces_.end()) {
    VXCORE_LOG_WARN("Cannot rename non-existent workspace: id=%s", id.c_str());
    return false;
  }

  it->second->name = name;
  SaveWorkspaces();
  VXCORE_LOG_INFO("Renamed workspace: id=%s, new_name=%s", id.c_str(), name.c_str());
  return true;
}

std::string WorkspaceManager::GetCurrentWorkspaceId() const { return current_workspace_id_; }

bool WorkspaceManager::SetCurrentWorkspaceId(const std::string &id) {
  if (workspaces_.find(id) == workspaces_.end()) {
    VXCORE_LOG_WARN("Cannot set current workspace to non-existent id: %s", id.c_str());
    return false;
  }

  current_workspace_id_ = id;
  SaveWorkspaces();
  VXCORE_LOG_INFO("Set current workspace: id=%s", id.c_str());
  return true;
}

bool WorkspaceManager::AddBufferToWorkspace(const std::string &ws_id, const std::string &buf_id) {
  auto it = workspaces_.find(ws_id);
  if (it == workspaces_.end()) {
    VXCORE_LOG_WARN("Cannot add buffer to non-existent workspace: ws_id=%s", ws_id.c_str());
    return false;
  }

  auto &buffer_ids = it->second->buffer_ids;
  // Check if buffer already in workspace
  if (std::find(buffer_ids.begin(), buffer_ids.end(), buf_id) != buffer_ids.end()) {
    VXCORE_LOG_DEBUG("Buffer already in workspace: ws_id=%s, buf_id=%s", ws_id.c_str(),
                     buf_id.c_str());
    return true;
  }

  buffer_ids.push_back(buf_id);
  SaveWorkspaces();
  VXCORE_LOG_INFO("Added buffer to workspace: ws_id=%s, buf_id=%s", ws_id.c_str(), buf_id.c_str());
  return true;
}

bool WorkspaceManager::RemoveBufferFromWorkspace(const std::string &ws_id,
                                                 const std::string &buf_id) {
  auto it = workspaces_.find(ws_id);
  if (it == workspaces_.end()) {
    VXCORE_LOG_WARN("Cannot remove buffer from non-existent workspace: ws_id=%s", ws_id.c_str());
    return false;
  }

  auto &buffer_ids = it->second->buffer_ids;
  auto buf_it = std::find(buffer_ids.begin(), buffer_ids.end(), buf_id);
  if (buf_it == buffer_ids.end()) {
    VXCORE_LOG_DEBUG("Buffer not in workspace: ws_id=%s, buf_id=%s", ws_id.c_str(), buf_id.c_str());
    return false;
  }

  buffer_ids.erase(buf_it);

  // If removing current buffer, clear current buffer
  if (it->second->current_buffer_id == buf_id) {
    it->second->current_buffer_id.clear();
  }

  SaveWorkspaces();
  VXCORE_LOG_INFO("Removed buffer from workspace: ws_id=%s, buf_id=%s", ws_id.c_str(),
                  buf_id.c_str());
  return true;
}

bool WorkspaceManager::SetCurrentBufferInWorkspace(const std::string &ws_id,
                                                   const std::string &buf_id) {
  auto it = workspaces_.find(ws_id);
  if (it == workspaces_.end()) {
    VXCORE_LOG_WARN("Cannot set current buffer in non-existent workspace: ws_id=%s", ws_id.c_str());
    return false;
  }

  // Note: We don't validate buf_id exists - BufferManager responsibility
  // We also don't check if buf_id is in buffer_ids - it might be added later

  it->second->current_buffer_id = buf_id;
  SaveWorkspaces();
  VXCORE_LOG_INFO("Set current buffer in workspace: ws_id=%s, buf_id=%s", ws_id.c_str(),
                  buf_id.c_str());
  return true;
}

}  // namespace vxcore
