#include "notebook.h"

#include <vxcore/notebook_json_keys.h>
#include <vxcore/vxcore_types.h>

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

#include "db/sqlite_metadata_store.h"
#include "event_manager.h"
#include "event_names.h"
#include "folder_manager.h"
#include "metadata_store.h"
#include "sync/sync_json_keys.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

VxCoreError NotebookEncryption::InstallNotebookKey(const EncryptedNote::KeyEnvelope &envelope,
                                                   EncryptedNote::Key &&notebook_key) {
  try {
    return InstallNotebookKey(envelope,
                              std::make_shared<EncryptedNote::Key>(std::move(notebook_key)));
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  }
}

VxCoreError NotebookEncryption::InstallNotebookKey(
    const EncryptedNote::KeyEnvelope &envelope,
    std::shared_ptr<const EncryptedNote::Key> notebook_key) {
  if (!EncryptedNote::IsCanonicalUuid(envelope.vault_id) ||
      !EncryptedNote::IsCanonicalUuid(envelope.notebook_id) ||
      !EncryptedNote::IsCanonicalUuid(envelope.notebook_key_id)) {
    return VXCORE_ERR_ENCRYPTION_FORMAT;
  }
  if (!notebook_key || !notebook_key->IsValid()) return VXCORE_ERR_ENCRYPTION_LOCKED;
  try {
    // Allocate before changing the ring; the final default selection cannot throw.
    KeyEntry candidate{envelope, std::move(notebook_key)};
    auto default_id = envelope.notebook_key_id;
    std::lock_guard<std::mutex> lock(mutex_);
    if (locking_) return VXCORE_ERR_INVALID_STATE;
    auto existing = notebook_keys_.find(default_id);
    if (existing != notebook_keys_.end()) {
      if (!EncryptedNote::SameEnvelope(existing->second.envelope, envelope) ||
          !EncryptedNote::KeysEqual(*existing->second.key, *candidate.key)) {
        return VXCORE_ERR_INVALID_STATE;
      }
    } else {
      notebook_keys_.emplace(default_id, std::move(candidate));
    }
    default_key_id_.swap(default_id);
    return VXCORE_OK;
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::length_error &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  }
}

VxCoreError NotebookEncryption::AcquireNotebookKey(
    std::shared_ptr<const EncryptedNote::Key> &out_key,
    EncryptedNote::KeyEnvelope *out_envelope, const EncryptedNote::ObjectHeader *header) const {
  out_key.reset();
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    if (locking_) return VXCORE_ERR_ENCRYPTION_LOCKED;
    if (header && (header->version != 1 && header->version != 2)) {
      return VXCORE_ERR_ENCRYPTION_FORMAT;
    }
    const auto found = notebook_keys_.find(header ? header->notebook_key_id : default_key_id_);
    if (found == notebook_keys_.end()) return VXCORE_ERR_ENCRYPTION_LOCKED;
    if (header && header->version == 2 &&
        !EncryptedNote::SameEnvelope(found->second.envelope, header->envelope)) {
      return VXCORE_ERR_ENCRYPTION_AUTH_FAILED;
    }
    if (out_envelope) *out_envelope = found->second.envelope;
    out_key = found->second.key;
    return VXCORE_OK;
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::length_error &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  }
}

VxCoreError NotebookEncryption::LockNotebookKey() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (locking_) return VXCORE_ERR_INVALID_STATE;
  for (const auto &entry : notebook_keys_) {
    if (entry.second.key.use_count() != 1) return VXCORE_ERR_INVALID_STATE;
  }
  locking_ = true;
  auto keys = std::move(notebook_keys_);
  default_key_id_.clear();
  lock.unlock();
  keys.clear();  // Secure owners wipe outside the mutex while new leases remain excluded.
  lock.lock();
  locking_ = false;
  return VXCORE_OK;
}

VxCoreError Notebook::EnsureEncryption(NotebookEncryption *&out_encryption) {
  out_encryption = nullptr;
  if (type_ != NotebookType::Bundled) {
    return VXCORE_ERR_UNSUPPORTED;
  }
  try {
    auto encryption = std::atomic_load(&encryption_);
    if (!encryption) {
      auto candidate = std::make_shared<NotebookEncryption>();
      if (std::atomic_compare_exchange_strong(&encryption_, &encryption, candidate)) {
        encryption = std::move(candidate);
      }
    }
    out_encryption = encryption.get();
    return VXCORE_OK;
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  }
}


const char *Notebook::kConfigFileName = "config.json";

TagNode::TagNode() : metadata(nlohmann::json::object()) {}

TagNode::TagNode(const std::string &name, const std::string &parent)
    : name(name), parent(parent), metadata(nlohmann::json::object()) {}

TagNode TagNode::FromJson(const nlohmann::json &json) {
  TagNode tag;
  if (json.contains(kJsonKeyName) && json[kJsonKeyName].is_string()) {
    tag.name = json[kJsonKeyName].get<std::string>();
  }
  if (json.contains(kJsonKeyParent) && json[kJsonKeyParent].is_string()) {
    tag.parent = json[kJsonKeyParent].get<std::string>();
  }
  if (json.contains(kJsonKeyMetadata) && json[kJsonKeyMetadata].is_object()) {
    tag.metadata = json[kJsonKeyMetadata];
  }
  return tag;
}

nlohmann::json TagNode::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  json[kJsonKeyName] = name;
  json[kJsonKeyParent] = parent;
  json[kJsonKeyMetadata] = metadata;
  return json;
}

NotebookConfig::NotebookConfig()
    : assets_folder("vx_assets"),
      recycle_bin_folder("vx_notebook/recycle_bin"),
      metadata(nlohmann::json::object()),
      tags_modified_utc(0) {}

NotebookConfig NotebookConfig::FromJson(const nlohmann::json &json) {
  NotebookConfig config;
  if (json.contains(kJsonKeyEncryptionInitialized)) {
    // get<bool>() deliberately rejects every nonboolean, including persisted null.
    config.encryption_initialized = json.at(kJsonKeyEncryptionInitialized).get<bool>();
  }
  if (json.contains(kJsonKeyId) && json[kJsonKeyId].is_string()) {
    config.id = json[kJsonKeyId].get<std::string>();
  }
  if (json.contains(kJsonKeyName) && json[kJsonKeyName].is_string()) {
    config.name = json[kJsonKeyName].get<std::string>();
  }
  if (json.contains(kJsonKeyDescription) && json[kJsonKeyDescription].is_string()) {
    config.description = json[kJsonKeyDescription].get<std::string>();
  }
  if (json.contains(kJsonKeyAssetsFolder) && json[kJsonKeyAssetsFolder].is_string()) {
    config.assets_folder = json[kJsonKeyAssetsFolder].get<std::string>();
  }
  if (json.contains(kJsonKeyRecycleBinFolder) && json[kJsonKeyRecycleBinFolder].is_string()) {
    const std::string recycle_bin_folder = json[kJsonKeyRecycleBinFolder].get<std::string>();
    if (!recycle_bin_folder.empty()) {
      config.recycle_bin_folder = recycle_bin_folder;
    }
  }
  // Note: attachmentsFolder is deprecated - attachments are now stored in assets folder
  if (json.contains(kJsonKeyMetadata) && json[kJsonKeyMetadata].is_object()) {
    config.metadata = json[kJsonKeyMetadata];
  }
  if (json.contains(kJsonKeyTags) && json[kJsonKeyTags].is_array()) {
    for (const auto &tag_json : json[kJsonKeyTags]) {
      config.tags.push_back(TagNode::FromJson(tag_json));
    }
  }
  if (json.contains(kJsonKeyTagsModifiedUtc) && json[kJsonKeyTagsModifiedUtc].is_number_integer()) {
    config.tags_modified_utc = json[kJsonKeyTagsModifiedUtc].get<int64_t>();
  }
  if (json.contains(kJsonKeyIgnored) && json[kJsonKeyIgnored].is_array()) {
    for (const auto &item : json[kJsonKeyIgnored]) {
      if (item.is_string()) {
        config.ignored.push_back(item.get<std::string>());
      }
    }
  }
  if (json.contains(kJsonKeySyncEnabled) && json[kJsonKeySyncEnabled].is_boolean()) {
    config.sync_enabled = json[kJsonKeySyncEnabled].get<bool>();
  }
  if (json.contains(kJsonKeySyncBackend) && json[kJsonKeySyncBackend].is_string()) {
    config.sync_backend = json[kJsonKeySyncBackend].get<std::string>();
  }
  if (json.contains(kJsonKeySyncRemoteUrl) && json[kJsonKeySyncRemoteUrl].is_string()) {
    config.sync_remote_url = json[kJsonKeySyncRemoteUrl].get<std::string>();
  }
  if (json.contains(kJsonKeyAutoSyncEnabled) && json[kJsonKeyAutoSyncEnabled].is_boolean()) {
    config.auto_sync_enabled = json[kJsonKeyAutoSyncEnabled].get<bool>();
  }
  return config;
}

nlohmann::json NotebookConfig::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  json[kJsonKeyId] = id;
  json[kJsonKeyName] = name;
  json[kJsonKeyDescription] = description;
  json[kJsonKeyAssetsFolder] = assets_folder;
  json[kJsonKeyRecycleBinFolder] = recycle_bin_folder;
  // Note: attachmentsFolder is deprecated - attachments are now stored in assets folder
  json[kJsonKeyMetadata] = metadata;
  nlohmann::json tags_array = nlohmann::json::array();
  for (const auto &tag : tags) {
    tags_array.push_back(tag.ToJson());
  }
  json[kJsonKeyTags] = std::move(tags_array);
  json[kJsonKeyTagsModifiedUtc] = tags_modified_utc;
  nlohmann::json ignored_array = nlohmann::json::array();
  for (const auto &pattern : ignored) {
    ignored_array.push_back(pattern);
  }
  json[kJsonKeyIgnored] = std::move(ignored_array);
  json[kJsonKeySyncEnabled] = sync_enabled;
  json[kJsonKeySyncBackend] = sync_backend;
  json[kJsonKeySyncRemoteUrl] = sync_remote_url;
  json[kJsonKeyAutoSyncEnabled] = auto_sync_enabled;
  if (encryption_initialized.has_value()) {
    json[kJsonKeyEncryptionInitialized] = *encryption_initialized;
  }
  return json;
}

NotebookRecord::NotebookRecord() : type(NotebookType::Bundled) {}

NotebookRecord NotebookRecord::FromJson(const nlohmann::json &json) {
  NotebookRecord record;
  if (json.contains(kJsonKeyId) && json[kJsonKeyId].is_string()) {
    record.id = json[kJsonKeyId].get<std::string>();
  }
  if (json.contains(kJsonKeyRootFolder) && json[kJsonKeyRootFolder].is_string()) {
    record.root_folder = json[kJsonKeyRootFolder].get<std::string>();
  }
  if (json.contains(kJsonKeyType) && json[kJsonKeyType].is_string()) {
    std::string typeStr = json[kJsonKeyType].get<std::string>();
    record.type = (typeStr == "raw") ? NotebookType::Raw : NotebookType::Bundled;
  }
  if (json.contains(kJsonKeyReadOnly) && json[kJsonKeyReadOnly].is_boolean()) {
    record.read_only = json[kJsonKeyReadOnly].get<bool>();
  }
  return record;
}

nlohmann::json NotebookRecord::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  json[kJsonKeyId] = id;
  json[kJsonKeyRootFolder] = root_folder;
  json[kJsonKeyType] = (type == NotebookType::Raw) ? "raw" : "bundled";
  json[kJsonKeyReadOnly] = read_only;
  return json;
}

Notebook::Notebook(const std::string &local_data_folder, const std::string &root_folder,
                   NotebookType type)
    : local_data_folder_(local_data_folder), root_folder_(root_folder), type_(type) {}

Notebook::~Notebook() = default;

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
  return ConcatenatePaths(GetLocalDataFolder(), "metadata.db");
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

void Notebook::SetLastSyncUtc(int64_t ts_millis) {
  if (!metadata_store_) {
    VXCORE_LOG_WARN("Notebook::SetLastSyncUtc: metadata_store_ is null, "
                    "skipping write for notebook=%s",
                    config_.id.c_str());
    return;
  }
  const bool ok =
      metadata_store_->SetNotebookMetadata("last_sync_utc", std::to_string(ts_millis));
  if (!ok) {
    VXCORE_LOG_WARN("Notebook::SetLastSyncUtc: write failed for notebook=%s",
                    config_.id.c_str());
  }
}

int64_t Notebook::GetLastSyncUtc() const {
  if (!metadata_store_) {
    return 0;
  }
  auto value = metadata_store_->GetNotebookMetadata("last_sync_utc");
  if (!value.has_value()) {
    return 0;
  }
  try {
    return std::stoll(value.value());
  } catch (...) {
    return 0;
  }
}

void Notebook::SetReadOnly(bool read_only) noexcept {
  read_only_ = read_only;
}

bool Notebook::IsReadOnly() const noexcept {
  return read_only_;
}

void Notebook::SetSyncApplyInProgress(bool active) noexcept {
  sync_apply_in_progress_.store(active, std::memory_order_release);
}

bool Notebook::IsSyncApplyInProgress() const noexcept {
  return sync_apply_in_progress_.load(std::memory_order_acquire);
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
  encryption_.reset();
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
  if (CheckWritable() != VXCORE_OK) { return CheckWritable(); }

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

VxCoreError Notebook::CreateTagPath(const std::string &tag_path) {
  if (CheckWritable() != VXCORE_OK) { return CheckWritable(); }

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
  if (CheckWritable() != VXCORE_OK) { return CheckWritable(); }

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
  if (CheckWritable() != VXCORE_OK) { return CheckWritable(); }

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

VxCoreError Notebook::FindFilesByTags(const std::vector<std::string> &tags, bool use_and,
                                      std::string &out_results_json) {
  if (!metadata_store_ || !metadata_store_->IsOpen()) {
    return VXCORE_ERR_INVALID_STATE;
  }

  auto results = use_and ? metadata_store_->FindFilesByTagsAnd(tags)
                         : metadata_store_->FindFilesByTagsOr(tags);

  nlohmann::json matches = nlohmann::json::array();
  for (const auto &result : results) {
    nlohmann::json match = nlohmann::json::object();
    match["filePath"] = result.file_path;
    match["fileName"] = result.file_name;
    match["tags"] = result.tags;
    matches.push_back(std::move(match));
  }

  nlohmann::json output = nlohmann::json::object();
  output["matchCount"] = static_cast<int>(matches.size());
  output["matches"] = std::move(matches);
  out_results_json = output.dump();
  return VXCORE_OK;
}

VxCoreError Notebook::CountFilesByTag(std::string &out_results_json) {
  if (!metadata_store_ || !metadata_store_->IsOpen()) {
    return VXCORE_ERR_INVALID_STATE;
  }

  auto counts = metadata_store_->CountFilesByTag();

  nlohmann::json results = nlohmann::json::array();
  for (const auto &pair : counts) {
    nlohmann::json entry = nlohmann::json::object();
    entry["tag"] = pair.first;
    entry["count"] = pair.second;
    results.push_back(std::move(entry));
  }

  out_results_json = results.dump();
  return VXCORE_OK;
}

}  // namespace vxcore
