#include "notebook.h"

#include <vxcore/vxcore_types.h>

#include <algorithm>
#include <unordered_map>

#include "db/sqlite_metadata_store.h"
#include "folder_manager.h"
#include "metadata_store.h"
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
      metadata(nlohmann::json::object()),
      tags_modified_utc(0) {}

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
  if (json.contains("tagsModifiedUtc") && json["tagsModifiedUtc"].is_number_integer()) {
    config.tags_modified_utc = json["tagsModifiedUtc"].get<int64_t>();
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
  json["tagsModifiedUtc"] = tags_modified_utc;
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
  return ConcatenatePaths(GetMetadataFolder(), "metadata.db");
}

VxCoreError Notebook::InitMetadataStore() {
  if (metadata_store_ && metadata_store_->IsOpen()) {
    return VXCORE_OK;  // Already initialized
  }

  auto store = std::make_unique<db::SqliteMetadataStore>();
  std::string db_path = GetDbPath();

  VXCORE_LOG_INFO("Initializing MetadataStore: notebook_id=%s, db_path=%s", config_.id.c_str(),
                  db_path.c_str());

  if (!store->Open(db_path)) {
    VXCORE_LOG_ERROR("Failed to open MetadataStore: %s", store->GetLastError().c_str());
    return VXCORE_ERR_IO;
  }

  metadata_store_ = std::move(store);
  return VXCORE_OK;
}

VxCoreError Notebook::SyncTagsToMetadataStore() {
  if (!metadata_store_ || !metadata_store_->IsOpen()) {
    return VXCORE_ERR_INVALID_STATE;
  }

  // 1. Get tags_synced_utc from DB
  int64_t tags_synced_utc = 0;
  auto synced_str = metadata_store_->GetNotebookMetadata("tags_synced_utc");
  if (synced_str.has_value()) {
    try {
      tags_synced_utc = std::stoll(synced_str.value());
    } catch (...) {
      tags_synced_utc = 0;
    }
  }

  // 2. Check if sync needed
  if (config_.tags_modified_utc > 0 && config_.tags_modified_utc <= tags_synced_utc) {
    VXCORE_LOG_DEBUG("Tags already synced: config=%lld, db=%lld", config_.tags_modified_utc,
                     tags_synced_utc);
    return VXCORE_OK;  // No sync needed
  }

  VXCORE_LOG_INFO("Syncing tags to MetadataStore: config=%lld, db=%lld", config_.tags_modified_utc,
                  tags_synced_utc);

  // 3. Begin transaction
  if (!metadata_store_->BeginTransaction()) {
    VXCORE_LOG_ERROR("Failed to begin transaction for tag sync");
    return VXCORE_ERR_UNKNOWN;
  }

  bool success = true;

  // 4. Sort tags by depth (root tags first, then children)
  // Build a map of tag name -> depth
  std::unordered_map<std::string, int> tag_depth;
  for (const auto &tag : config_.tags) {
    if (tag.parent.empty()) {
      tag_depth[tag.name] = 0;
    }
  }

  // Iteratively compute depths for tags with parents
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &tag : config_.tags) {
      if (tag_depth.find(tag.name) != tag_depth.end()) {
        continue;
      }
      if (!tag.parent.empty() && tag_depth.find(tag.parent) != tag_depth.end()) {
        tag_depth[tag.name] = tag_depth[tag.parent] + 1;
        changed = true;
      }
    }
  }

  // Sort tags by depth
  std::vector<const TagNode *> sorted_tags;
  sorted_tags.reserve(config_.tags.size());
  for (const auto &tag : config_.tags) {
    sorted_tags.push_back(&tag);
  }
  std::sort(sorted_tags.begin(), sorted_tags.end(),
            [&tag_depth](const TagNode *a, const TagNode *b) {
              int da = tag_depth.count(a->name) ? tag_depth[a->name] : 999;
              int db = tag_depth.count(b->name) ? tag_depth[b->name] : 999;
              return da < db;
            });

  // 5. Create/update tags in depth order
  for (const TagNode *tag : sorted_tags) {
    StoreTagRecord store_tag;
    store_tag.name = tag->name;
    store_tag.parent_name = tag->parent;
    store_tag.metadata = tag->metadata.dump();

    if (!metadata_store_->CreateOrUpdateTag(store_tag)) {
      VXCORE_LOG_ERROR("Failed to sync tag: %s", tag->name.c_str());
      success = false;
      break;
    }
  }

  // 6. Delete orphan tags (tags in DB but not in config)
  if (success) {
    auto db_tags = metadata_store_->ListTags();
    for (const auto &db_tag : db_tags) {
      if (tag_depth.find(db_tag.name) == tag_depth.end()) {
        VXCORE_LOG_INFO("Deleting orphan tag: %s", db_tag.name.c_str());
        if (!metadata_store_->DeleteTag(db_tag.name)) {
          VXCORE_LOG_WARN("Failed to delete orphan tag: %s", db_tag.name.c_str());
          // Continue anyway - orphan cleanup is best-effort
        }
      }
    }
  }

  // 7. Commit or rollback
  if (success) {
    if (!metadata_store_->CommitTransaction()) {
      VXCORE_LOG_ERROR("Failed to commit tag sync transaction");
      return VXCORE_ERR_UNKNOWN;
    }
  } else {
    metadata_store_->RollbackTransaction();
    return VXCORE_ERR_UNKNOWN;
  }

  // 8. Update tags_synced_utc in DB
  int64_t sync_time =
      config_.tags_modified_utc > 0 ? config_.tags_modified_utc : GetCurrentTimestampMillis();
  metadata_store_->SetNotebookMetadata("tags_synced_utc", std::to_string(sync_time));

  // 9. If config had tags_modified_utc=0, set it to current time and save
  if (config_.tags_modified_utc == 0 && !config_.tags.empty()) {
    config_.tags_modified_utc = sync_time;
    // Note: UpdateConfig is virtual, will save to appropriate location
    auto err = UpdateConfig(config_);
    if (err != VXCORE_OK) {
      VXCORE_LOG_WARN("Failed to update config after tag sync: error=%d", err);
      // Don't fail - sync itself succeeded
    }
  }

  VXCORE_LOG_INFO("Tag sync completed successfully");
  return VXCORE_OK;
}

void Notebook::Close() {
  VXCORE_LOG_INFO("Closing notebook: id=%s", config_.id.c_str());

  // Close MetadataStore first to release DB file lock
  if (metadata_store_) {
    metadata_store_->Close();
    metadata_store_.reset();
  }

  // Reset folder manager
  folder_manager_.reset();
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
  config_.tags_modified_utc = GetCurrentTimestampMillis();

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

  config_.tags_modified_utc = GetCurrentTimestampMillis();
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

  config_.tags_modified_utc = GetCurrentTimestampMillis();
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
