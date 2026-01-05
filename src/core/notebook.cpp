#include "notebook.h"

#include <vxcore/vxcore_types.h>

#include <algorithm>

#include "folder_manager.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

const char *Notebook::kConfigFileName = "config.json";

TagNode::TagNode() : metadata(nlohmann::json::object()) {}

TagNode::TagNode(const std::string &name, const std::string &parent)
    : name(name), parent(parent), metadata(nlohmann::json::object()) {}

TagNode TagNode::FromJson(const nlohmann::json &json) {
  TagNode tag;
  if (json.contains("name") && json["name"].is_string()) {
    tag.name = json["name"].get<std::string>();
  }
  if (json.contains("parent") && json["parent"].is_string()) {
    tag.parent = json["parent"].get<std::string>();
  }
  if (json.contains("metadata") && json["metadata"].is_object()) {
    tag.metadata = json["metadata"];
  }
  return tag;
}

nlohmann::json TagNode::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  json["name"] = name;
  json["parent"] = parent;
  json["metadata"] = metadata;
  return json;
}

NotebookConfig::NotebookConfig()
    : assets_folder("vx_assets"),
      attachments_folder("vx_attachments"),
      metadata(nlohmann::json::object()) {}

NotebookConfig NotebookConfig::FromJson(const nlohmann::json &json) {
  NotebookConfig config;
  if (json.contains("id") && json["id"].is_string()) {
    config.id = json["id"].get<std::string>();
  }
  if (json.contains("name") && json["name"].is_string()) {
    config.name = json["name"].get<std::string>();
  }
  if (json.contains("description") && json["description"].is_string()) {
    config.description = json["description"].get<std::string>();
  }
  if (json.contains("assetsFolder") && json["assetsFolder"].is_string()) {
    config.assets_folder = json["assetsFolder"].get<std::string>();
  }
  if (json.contains("attachmentsFolder") && json["attachmentsFolder"].is_string()) {
    config.attachments_folder = json["attachmentsFolder"].get<std::string>();
  }
  if (json.contains("metadata") && json["metadata"].is_object()) {
    config.metadata = json["metadata"];
  }
  if (json.contains("tags") && json["tags"].is_array()) {
    for (const auto &tag_json : json["tags"]) {
      config.tags.push_back(TagNode::FromJson(tag_json));
    }
  }
  return config;
}

nlohmann::json NotebookConfig::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  json["id"] = id;
  json["name"] = name;
  json["description"] = description;
  json["assetsFolder"] = assets_folder;
  json["attachmentsFolder"] = attachments_folder;
  json["metadata"] = metadata;
  nlohmann::json tags_array = nlohmann::json::array();
  for (const auto &tag : tags) {
    tags_array.push_back(tag.ToJson());
  }
  json["tags"] = std::move(tags_array);
  return json;
}

NotebookRecord::NotebookRecord() : type(NotebookType::Bundled) {}

NotebookRecord NotebookRecord::FromJson(const nlohmann::json &json) {
  NotebookRecord record;
  if (json.contains("id") && json["id"].is_string()) {
    record.id = json["id"].get<std::string>();
  }
  if (json.contains("rootFolder") && json["rootFolder"].is_string()) {
    record.root_folder = json["rootFolder"].get<std::string>();
  }
  if (json.contains("type") && json["type"].is_string()) {
    std::string typeStr = json["type"].get<std::string>();
    record.type = (typeStr == "raw") ? NotebookType::Raw : NotebookType::Bundled;
  }
  return record;
}

nlohmann::json NotebookRecord::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  json["id"] = id;
  json["rootFolder"] = root_folder;
  json["type"] = (type == NotebookType::Raw) ? "raw" : "bundled";
  return json;
}

Notebook::Notebook(const std::string &local_data_folder, const std::string &root_folder,
                   NotebookType type)
    : local_data_folder_(local_data_folder), root_folder_(root_folder), type_(type) {}

void Notebook::EnsureId() {
  if (config_.id.empty()) {
    config_.id = GenerateUUID();
  }
}

std::string Notebook::GetLocalDataFolder() const {
  auto notebooks = ConcatenatePaths(local_data_folder_, "notebooks");
  return ConcatenatePaths(notebooks, config_.id);
}

std::string Notebook::GetDbPath() const {
  return ConcatenatePaths(GetLocalDataFolder(), "notebook.db");
}

TagNode *Notebook::FindTag(const std::string &tag_name) {
  for (auto &tag : config_.tags) {
    if (tag.name == tag_name) {
      return &tag;
    }
  }
  return nullptr;
}

VxCoreError Notebook::CreateTag(const std::string &tag_name, const std::string &parent_tag) {
  if (tag_name.empty()) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  if (FindTag(tag_name)) {
    return VXCORE_ERR_ALREADY_EXISTS;
  }

  if (!parent_tag.empty() && !FindTag(parent_tag)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  config_.tags.emplace_back(tag_name, parent_tag);

  auto err = UpdateConfig(config_);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR(
        "Failed to update notebook config after creating tag: notebook_id=%s, tag_name=%s, "
        "error=%d",
        config_.id.c_str(), tag_name.c_str(), err);
    return err;
  }

  return VXCORE_OK;
}

VxCoreError Notebook::CreateFolderPath(const std::string &folder_path, std::string &out_folder_id) {
  if (folder_path.empty()) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  if (!folder_manager_) {
    return VXCORE_ERR_INVALID_STATE;
  }

  std::vector<std::string> path_components = SplitPathComponents(folder_path);
  if (path_components.empty()) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  std::string current_parent = ".";
  std::string folder_id;

  for (size_t i = 0; i < path_components.size(); ++i) {
    const std::string &folder_name = path_components[i];
    if (folder_name.empty()) {
      return VXCORE_ERR_INVALID_PARAM;
    }

    VxCoreError create_err = folder_manager_->CreateFolder(current_parent, folder_name, folder_id);
    if (create_err != VXCORE_OK && create_err != VXCORE_ERR_ALREADY_EXISTS) {
      return create_err;
    }

    current_parent = ConcatenatePaths(current_parent, folder_name);
  }

  out_folder_id = folder_id;
  return VXCORE_OK;
}

VxCoreError Notebook::CreateTagPath(const std::string &tag_path) {
  if (tag_path.empty()) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  std::vector<std::string> path_components = SplitPathComponents(tag_path);
  if (path_components.empty()) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  for (size_t i = 0; i < path_components.size(); ++i) {
    const std::string &tag_name = path_components[i];
    if (tag_name.empty()) {
      return VXCORE_ERR_INVALID_PARAM;
    }

    if (!FindTag(tag_name)) {
      VxCoreError err = CreateTag(tag_name, (i > 0) ? path_components[i - 1] : "");
      if (err != VXCORE_OK && err != VXCORE_ERR_ALREADY_EXISTS) {
        return err;
      }
    }
  }

  return VXCORE_OK;
}

VxCoreError Notebook::DeleteTag(const std::string &tag_name) {
  if (tag_name.empty()) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  if (!FindTag(tag_name)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  std::vector<std::string> tags_to_delete;
  tags_to_delete.push_back(tag_name);

  std::vector<std::string> to_process = {tag_name};
  while (!to_process.empty()) {
    std::string current = to_process.back();
    to_process.pop_back();

    for (const auto &tag : config_.tags) {
      if (tag.parent == current) {
        tags_to_delete.push_back(tag.name);
        to_process.push_back(tag.name);
      }
    }
  }

  if (folder_manager_) {
    folder_manager_->IterateAllFiles(
        [this, &tags_to_delete](const std::string &folder_path, const FileRecord &file) {
          auto tags = file.tags;
          bool modified = false;

          for (const auto &tag_to_delete : tags_to_delete) {
            auto tag_it = std::find(tags.begin(), tags.end(), tag_to_delete);
            if (tag_it != tags.end()) {
              tags.erase(tag_it);
              modified = true;
            }
          }

          if (modified) {
            nlohmann::json tags_json = nlohmann::json(tags);
            std::string tags_str = tags_json.dump();
            folder_manager_->UpdateFileTags(ConcatenatePaths(folder_path, file.name), tags_str);
          }
          return true;
        });
  }

  for (const auto &tag_to_delete : tags_to_delete) {
    auto tag_it =
        std::find_if(config_.tags.begin(), config_.tags.end(),
                     [&tag_to_delete](const TagNode &tag) { return tag.name == tag_to_delete; });
    if (tag_it != config_.tags.end()) {
      config_.tags.erase(tag_it);
    }
  }

  auto err = UpdateConfig(config_);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR(
        "Failed to update notebook config after deleting tag: notebook_id=%s, tag_name=%s, "
        "error=%d",
        config_.id.c_str(), tag_name.c_str(), err);
    return err;
  }

  return VXCORE_OK;
}

VxCoreError Notebook::MoveTag(const std::string &tag_name, const std::string &parent_tag) {
  if (tag_name.empty()) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  if (parent_tag == tag_name) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  TagNode *tag = FindTag(tag_name);
  if (!tag) {
    return VXCORE_ERR_NOT_FOUND;
  }

  if (!parent_tag.empty() && !FindTag(parent_tag)) {
    return VXCORE_ERR_NOT_FOUND;
  }

  std::string current_parent = parent_tag;
  while (!current_parent.empty()) {
    if (current_parent == tag_name) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    TagNode *parent_node = FindTag(current_parent);
    if (!parent_node) {
      break;
    }
    current_parent = parent_node->parent;
  }

  tag->parent = parent_tag;

  auto err = UpdateConfig(config_);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR(
        "Failed to update notebook config after moving tag: notebook_id=%s, tag_name=%s, "
        "error=%d",
        config_.id.c_str(), tag_name.c_str(), err);
    return err;
  }

  return VXCORE_OK;
}

VxCoreError Notebook::GetTags(std::string &out_tags_json) const {
  nlohmann::json tags_array = nlohmann::json::array();
  for (const auto &tag : config_.tags) {
    tags_array.push_back(tag.ToJson());
  }
  out_tags_json = tags_array.dump();
  return VXCORE_OK;
}

std::string Notebook::GetCleanRelativePath(const std::string &path) const {
  const auto clean_path = CleanPath(path);
  if (IsRelativePath(clean_path)) {
    return clean_path;
  }
  return RelativePath(root_folder_, clean_path);
}

std::string Notebook::GetAbsolutePath(const std::string &relative_path) const {
  return ConcatenatePaths(root_folder_, relative_path);
}

}  // namespace vxcore
