#include "vxcore_session_config.h"

namespace vxcore {

VxCoreSessionConfig VxCoreSessionConfig::FromJson(const nlohmann::json &json) {
  VxCoreSessionConfig config;
  if (json.contains("notebooks") && json["notebooks"].is_array()) {
    for (const auto &item : json["notebooks"]) {
      config.notebooks.push_back(NotebookRecord::FromJson(item));
    }
  }
  if (json.contains("workspaces") && json["workspaces"].is_array()) {
    for (const auto &item : json["workspaces"]) {
      config.workspaces.push_back(WorkspaceRecord::FromJson(item));
    }
  }
  if (json.contains("buffers") && json["buffers"].is_array()) {
    for (const auto &item : json["buffers"]) {
      config.buffers.push_back(BufferRecord::FromJson(item));
    }
  }
  if (json.contains("currentWorkspaceId") && json["currentWorkspaceId"].is_string()) {
    config.current_workspace_id = json["currentWorkspaceId"].get<std::string>();
  }
  return config;
}

nlohmann::json VxCoreSessionConfig::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  nlohmann::json notebooksArray = nlohmann::json::array();
  for (const auto &record : notebooks) {
    notebooksArray.push_back(record.ToJson());
  }
  json["notebooks"] = notebooksArray;

  nlohmann::json workspacesArray = nlohmann::json::array();
  for (const auto &record : workspaces) {
    workspacesArray.push_back(record.ToJson());
  }
  json["workspaces"] = workspacesArray;

  nlohmann::json buffersArray = nlohmann::json::array();
  for (const auto &record : buffers) {
    buffersArray.push_back(record.ToJson());
  }
  json["buffers"] = buffersArray;

  json["currentWorkspaceId"] = current_workspace_id;

  return json;
}

}  // namespace vxcore
