#ifndef VXCORE_NOTEBOOK_H
#define VXCORE_NOTEBOOK_H

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "encryptednote.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class EventManager;
class FolderManager;
class MetadataStore;
class NotebookManager;

enum class NotebookType { Bundled, Raw };

struct TagNode {
  std::string name;
  std::string parent;
  nlohmann::json metadata;

  TagNode();
  TagNode(const std::string &name, const std::string &parent = std::string());

  static TagNode FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

// Note: Read-only state is INTENTIONALLY NOT a NotebookConfig field.
// Read-only is a per-device property (set on Notebook runtime via
// Notebook::SetReadOnly, persisted in NotebookRecord). Putting it in
// NotebookConfig would sync the flag across all devices, including
// ones that have a valid PAT — which is the opposite of intent.
// See .sisyphus/plans/open-notebook-remote-readonly.md fork B.
struct VXCORE_API NotebookConfig {
  std::string id;
  std::string name;
  std::string description;
  std::string assets_folder;
  std::string recycle_bin_folder;
  nlohmann::json metadata;
  std::vector<std::string> ignored;
  std::vector<TagNode> tags;
  int64_t tags_modified_utc;
  bool sync_enabled = false;
  std::string sync_backend;
  std::string sync_remote_url;
  bool auto_sync_enabled = true;
  // Unknown until new-notebook creation or explicit legacy reconciliation.
  std::optional<bool> encryption_initialized;

  NotebookConfig();

  static NotebookConfig FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

struct VXCORE_API NotebookRecord {
  std::string id;
  std::string root_folder;
  NotebookType type;
  bool read_only = false;

  NotebookRecord();

  static NotebookRecord FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

// Notebook-owned session keys. The codec owns cryptography; this ring owns leases.
class NotebookEncryption final {
 public:
  NotebookEncryption() = default;
  NotebookEncryption(const NotebookEncryption &) = delete;
  NotebookEncryption &operator=(const NotebookEncryption &) = delete;

  // Install authenticated keys only. Existing entries are verified, never replaced.
  // Every protected job/buffer retains an NK lease through its complete lifetime.
  VxCoreError InstallNotebookKey(const EncryptedNote::KeyEnvelope &envelope,
                                EncryptedNote::Key &&notebook_key);
  VxCoreError InstallNotebookKey(const EncryptedNote::KeyEnvelope &envelope,
                                std::shared_ptr<const EncryptedNote::Key> notebook_key);
  VxCoreError AcquireNotebookKey(std::shared_ptr<const EncryptedNote::Key> &out_key,
                                EncryptedNote::KeyEnvelope *out_envelope = nullptr,
                                const EncryptedNote::ObjectHeader *header = nullptr) const;
  VxCoreError LockNotebookKey();

 private:
  friend class NotebookManager;  // Coordinated Lock All barrier only.
  struct KeyEntry {
    EncryptedNote::KeyEnvelope envelope;
    std::shared_ptr<const EncryptedNote::Key> key;
  };
  mutable std::mutex mutex_;
  bool locking_ = false;
  std::map<std::string, KeyEntry> notebook_keys_;
  std::string default_key_id_;
};

class Notebook {
 public:
  friend class NotebookManager;
  virtual ~Notebook();

  const std::string &GetId() const { return config_.id; }
  const std::string &GetRootFolder() const { return root_folder_; }
  NotebookType GetType() const { return type_; }
  std::string GetTypeStr() const { return type_ == NotebookType::Raw ? "raw" : "bundled"; }
  const NotebookConfig &GetConfig() const { return config_; }
  virtual VxCoreError UpdateConfig(const NotebookConfig &config) = 0;
  std::string GetLocalDataFolder() const;
  virtual std::string GetMetadataFolder() const = 0;

  // Recycle bin operations (bundled notebooks only)
  // Returns the path to the recycle bin folder, or empty string if not supported.
  virtual std::string GetRecycleBinPath() const = 0;
  // Empties the recycle bin by deleting all files and folders in it.
  // Returns VXCORE_OK on success, VXCORE_ERR_NOT_SUPPORTED for raw notebooks.
  virtual VxCoreError EmptyRecycleBin() = 0;

  FolderManager *GetFolderManager() { return folder_manager_.get(); }
  MetadataStore *GetMetadataStore() { return metadata_store_.get(); }

  void SetEventManager(EventManager *event_manager) { event_manager_ = event_manager; }

  // Per-device "last successful git sync" timestamp, persisted in metadata DB
  // (NOT in NotebookConfig JSON -- that file is inside the synced tree, which
  // would cause a self-sync loop). Units: int64 milliseconds since Unix epoch.
  // 0 means never synced (or read error). Best-effort: write failures are
  // logged but not propagated, mirroring the tags_synced_utc pattern.
  void SetLastSyncUtc(int64_t ts_millis);
  int64_t GetLastSyncUtc() const;

  // Per-device read-only flag. When set to true, the notebook cannot be
  // mutated (no file/folder edits, saves, or sync registration).
  // This is a runtime flag, persisted in NotebookRecord (session state).
  // Both bundled and raw notebooks support this flag.
  void SetReadOnly(bool read_only) noexcept;
  bool IsReadOnly() const noexcept;

  // Runtime-only reservation. Callers drain existing writers before setting it.
  void SetSyncApplyInProgress(bool active) noexcept;
  bool IsSyncApplyInProgress() const noexcept;

  // Cached runtime recovery interlock, independent of the user's read-only setting.
  // Callers serialize changes with the notebook maintenance/IO lease.
  VxCoreError CheckWritable() const noexcept {
    if (encryption_recovery_required_) return VXCORE_ERR_ENCRYPTION_RECOVERY_REQUIRED;
    if (read_only_) return VXCORE_ERR_READ_ONLY;
    return IsSyncApplyInProgress() ? VXCORE_ERR_SYNC_IN_PROGRESS : VXCORE_OK;
  }
  bool IsEncryptionRecoveryRequired() const noexcept { return encryption_recovery_required_; }
  void SetEncryptionRecoveryRequired(bool required) noexcept {
    encryption_recovery_required_ = required;
  }

  // Closes the notebook, releasing all resources (DB connections, etc.).
  // Must be called before deleting the notebook's local data folder. As with DB/IO
  // operations, callers quiesce protected work before closing; extant key leases remain
  // memory-safe but must be released before a Lock All operation can report success.
  void Close();

  // Used only after protected dispatch; ordinary IO never queries this owner.
  // Atomic snapshots also make concurrent encryption-worker first use safe.
  NotebookEncryption *GetEncryption() const noexcept {
    return std::atomic_load(&encryption_).get();
  }
  VxCoreError EnsureEncryption(NotebookEncryption *&out_encryption);

  virtual VxCoreError CreateTag(const std::string &tag_name, const std::string &parent_tag = "");
  virtual VxCoreError CreateTagPath(const std::string &tag_path);
  virtual VxCoreError DeleteTag(const std::string &tag_name);
  virtual VxCoreError MoveTag(const std::string &tag_name, const std::string &parent_tag);
  virtual VxCoreError GetTags(std::string &out_tags_json) const;
  TagNode *FindTag(const std::string &tag_name);

  // Direct DB-backed tag queries (bypasses SearchManager for efficiency)
  virtual VxCoreError FindFilesByTags(const std::vector<std::string> &tags, bool use_and,
                              std::string &out_results_json);
  virtual VxCoreError CountFilesByTag(std::string &out_results_json);

  // Rebuild the metadata cache from ground truth (config files).
  // Returns VXCORE_OK on success.
  virtual VxCoreError RebuildCache() = 0;

  // Clean and get path related to notebook root folder.
  // Returns null string if |path| is not under notebook root folder.
  std::string GetCleanRelativePath(const std::string &path) const;

  std::string GetAbsolutePath(const std::string &relative_path) const;

 protected:
  Notebook(const std::string &local_data_folder, const std::string &root_folder, NotebookType type);

  void EnsureId();

  // Initialize and open the MetadataStore for this notebook
  // Returns VXCORE_OK on success, or error code on failure
  VxCoreError InitMetadataStore();

  // Syncs tags from NotebookConfig to MetadataStore if config is newer
  // Returns VXCORE_OK on success or if no sync needed
  VxCoreError SyncTagsToMetadataStore();

  std::string GetDbPath() const;
  virtual std::string GetConfigPath() const = 0;

  const std::string local_data_folder_;
  const std::string root_folder_;
  const NotebookType type_;

  NotebookConfig config_;
  std::unique_ptr<FolderManager> folder_manager_;
  std::unique_ptr<MetadataStore> metadata_store_;
  EventManager *event_manager_ = nullptr;
  bool read_only_ = false;
  bool encryption_recovery_required_ = false;
  std::atomic<bool> sync_apply_in_progress_{false};
  // The sparse session registry retains this owner across Close(), so live key
  // leases cannot disappear from Lock All preflight when a notebook is closed.
  std::shared_ptr<NotebookEncryption> encryption_;

  static const char *kConfigFileName;
};

}  // namespace vxcore

#endif
