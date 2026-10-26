#include "vxcore_session_config.h"

#include <algorithm>
#include <string_view>
#include <unordered_set>

namespace vxcore {
namespace {

// Membership belongs to the complete runtime snapshot, not the persisted subset.
// Borrow IDs only while its workspace records remain unchanged.
struct PreviewWorkspaceFilter {
  PreviewWorkspaceFilter(const std::vector<WorkspaceRecord> &workspaces,
                         const std::string &current_workspace_id) {
    has_previews = std::any_of(workspaces.begin(), workspaces.end(),
                               [](const auto &record) { return record.is_preview; });
    if (!has_previews) {
      return;
    }
    for (const auto &record : workspaces) {
      auto &ids = record.is_preview ? preview_buffer_ids : ordinary_buffer_ids;
      ids.insert(record.buffer_ids.begin(), record.buffer_ids.end());
      if (record.is_preview) {
        if (record.id == current_workspace_id) {
          current_is_preview = true;
        }
      } else if (!first_ordinary_workspace) {
        first_ordinary_workspace = &record;
      }
    }
  }

  bool IsPreviewOnlyBuffer(const std::string &id) const {
    return preview_buffer_ids.count(id) != 0 && ordinary_buffer_ids.count(id) == 0;
  }

  bool has_previews = false;
  bool current_is_preview = false;
  const WorkspaceRecord *first_ordinary_workspace = nullptr;
  std::unordered_set<std::string_view> preview_buffer_ids;
  std::unordered_set<std::string_view> ordinary_buffer_ids;
};

}  // namespace

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
  const PreviewWorkspaceFilter previews(config.workspaces, config.current_workspace_id);
  if (previews.has_previews) {
    if (previews.current_is_preview) {
      config.current_workspace_id =
          previews.first_ordinary_workspace ? previews.first_ordinary_workspace->id : "";
    }
    config.buffers.erase(std::remove_if(config.buffers.begin(), config.buffers.end(),
                                        [&previews](const auto &record) {
                                          return previews.IsPreviewOnlyBuffer(record.id);
                                        }),
                         config.buffers.end());
    // Remove workspaces last: the membership filter borrows their buffer IDs.
    config.workspaces.erase(std::remove_if(config.workspaces.begin(), config.workspaces.end(),
                                           [](const auto &record) { return record.is_preview; }),
                            config.workspaces.end());
  }
  return config;
}

nlohmann::json VxCoreSessionConfig::ToJson() const {
  const PreviewWorkspaceFilter previews(workspaces, current_workspace_id);
  nlohmann::json json = nlohmann::json::object();
  nlohmann::json notebooksArray = nlohmann::json::array();
  for (const auto &record : notebooks) {
    notebooksArray.push_back(record.ToJson());
  }
  json["notebooks"] = notebooksArray;

  nlohmann::json workspacesArray = nlohmann::json::array();
  for (const auto &record : workspaces) {
    if (!record.is_preview) {
      workspacesArray.push_back(record.ToJson());
    }
  }
  json["workspaces"] = workspacesArray;

  nlohmann::json buffersArray = nlohmann::json::array();
  for (const auto &record : buffers) {
    if (!previews.IsPreviewOnlyBuffer(record.id)) {
      buffersArray.push_back(record.ToJson());
    }
  }
  json["buffers"] = buffersArray;

  if (previews.current_is_preview) {
    json["currentWorkspaceId"] =
        previews.first_ordinary_workspace ? previews.first_ordinary_workspace->id : "";
  } else {
    json["currentWorkspaceId"] = current_workspace_id;
  }

  return json;
}

}  // namespace vxcore
