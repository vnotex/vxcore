#ifndef VXCORE_NOTEBOOK_H
#define VXCORE_NOTEBOOK_H

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <istream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

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

// Private-core authenticated storage. None of these owners exist on plaintext paths.
class NotebookEncryption final {
 public:
  static constexpr size_t kKeyBytes = 32;
  static constexpr size_t kRecordBytes = 65536;
  static constexpr size_t kMaxHeaderBytes = 4096;

  class Key final {
   public:
    Key() noexcept = default;
    ~Key();
    Key(Key &&other) noexcept;
    Key &operator=(Key &&other) noexcept;
    Key(const Key &) = delete;
    Key &operator=(const Key &) = delete;
    bool IsValid() const noexcept { return data_ != nullptr; }
    void Reset() noexcept;

   private:
    friend class NotebookEncryption;
    unsigned char *data_ = nullptr;
  };

  // Owns application plaintext; destruction/Reset wipes the entire allocation.
  // Data is borrowed until this owner is moved/reset/destroyed. No key exposes Data().
  class SecureBytes final {
   public:
    SecureBytes() noexcept = default;
    ~SecureBytes();
    SecureBytes(SecureBytes &&other) noexcept;
    SecureBytes &operator=(SecureBytes &&other) noexcept;
    SecureBytes(const SecureBytes &) = delete;
    SecureBytes &operator=(const SecureBytes &) = delete;
    const unsigned char *Data() const noexcept { return data_; }
    size_t Size() const noexcept { return size_; }
    void Reset() noexcept;

   private:
    friend class NotebookEncryption;
    VxCoreError Allocate(size_t capacity);
    unsigned char *data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
  };

  // These value types contain public identity/ciphertext only, never raw keys.
  struct WrappedKey {
    std::array<unsigned char, 24> nonce{};
    std::array<unsigned char, 48> ciphertext{};
  };
  struct KeyEnvelope {
    std::string vault_id;
    std::string notebook_id;
    std::string notebook_key_id;
    std::array<unsigned char, 16> salt{};
    WrappedKey master_key;
    WrappedKey notebook_key;
  };
  struct ObjectHeader {
    std::string kind;
    std::string document_id;
    std::string object_id;
    // Present only on note/backup objects. Resources cannot carry a wrapped key.
    std::string notebook_key_id;
    WrappedKey note_key;
  };
  using Fingerprint = std::array<unsigned char, 32>;

  NotebookEncryption() = default;
  ~NotebookEncryption() = default;
  NotebookEncryption(const NotebookEncryption &) = delete;
  NotebookEncryption &operator=(const NotebookEncryption &) = delete;

  static bool IsCanonicalUuid(const std::string &value) noexcept;
  static VxCoreError GenerateKey(Key &out_key);
  static bool KeysEqual(const Key &left, const Key &right) noexcept;
  static VxCoreError GenerateIdentity(std::string &out_uuid);
  static void WipeBytes(std::vector<uint8_t> &bytes) noexcept;
  static void WipeString(std::string &text) noexcept;
  static void WipeJson(nlohmann::json &json) noexcept;

  // KDF is performed here, outside any registry/IO lock. Password input is borrowed:
  // the caller must erase its own storage; our guarded copy and KEK are erased promptly.
  // Failure leaves envelope/key outputs unchanged. No method here publishes a key file.
  static VxCoreError PrepareNewKeys(const std::string &notebook_id, const void *password,
                                    size_t password_size, KeyEnvelope &out_envelope,
                                    Key &out_master_key, Key &out_notebook_key);
  static VxCoreError PrepareNotebookKeys(const KeyEnvelope &authenticated_source,
                                         const Key &master_key, const std::string &notebook_id,
                                         KeyEnvelope &out_envelope, Key &out_notebook_key);
  static VxCoreError UnlockKeys(const KeyEnvelope &envelope, const std::string &notebook_id,
                                const void *password, size_t password_size,
                                Key &out_master_key, Key &out_notebook_key);
  static VxCoreError EncodeKeyEnvelope(const KeyEnvelope &envelope, std::string &out_bytes);
  static VxCoreError DecodeKeyEnvelope(const void *bytes, size_t size, KeyEnvelope &out_envelope);
  static VxCoreError ReadKeyEnvelope(const std::filesystem::path &path, KeyEnvelope &out_envelope);

  // The caller supplies fresh object IDs for new snapshots; every encryption also
  // creates a fresh stream header and every key wrap creates a fresh nonce.
  static VxCoreError WrapNoteKey(const std::string &notebook_id,
                                 const std::string &notebook_key_id, const Key &notebook_key,
                                 const Key &note_key, ObjectHeader &in_out_header);
  static VxCoreError UnwrapNoteKey(const std::string &notebook_id,
                                   const std::string &notebook_key_id, const Key &notebook_key,
                                   const ObjectHeader &header, Key &out_note_key);
  // Header-only reads are UNAUTHENTICATED metadata. For a note, unwrap its DK first,
  // then pass the same complete header to ReadObject; changes between reads fail closed.
  static VxCoreError ReadObjectHeader(const std::filesystem::path &path, ObjectHeader &out_header);
  static VxCoreError EncryptObject(const std::filesystem::path &path, const ObjectHeader &header,
                                   const Key &data_key, std::istream &plaintext,
                                   Fingerprint *out_fingerprint = nullptr);
  static VxCoreError EncryptObjectBytes(const std::filesystem::path &path,
                                        const ObjectHeader &header, const Key &data_key,
                                        const void *bytes, size_t size);
  // All failures clear out_plaintext. No plaintext is returned before FINAL and EOF.
  static VxCoreError ReadObject(const std::filesystem::path &path, const ObjectHeader &expected,
                                const Key &data_key, size_t max_bytes, SecureBytes &out_plaintext,
                                Fingerprint *out_fingerprint = nullptr);
  // Conversion staging verification: authenticate FINAL/EOF with bounded memory,
  // discard plaintext, and optionally hash the authenticated plaintext stream.
  static VxCoreError VerifyObject(const std::filesystem::path &path,
                                  const ObjectHeader &expected, const Key &data_key,
                                  Fingerprint *out_plaintext_fingerprint = nullptr);
  // Explicit plaintext release only: stages at the chosen destination, then publishes
  // after complete authentication. Caller enforces export consent/destination policy.
  static VxCoreError ExportObject(const std::filesystem::path &path, const ObjectHeader &expected,
                                  const Key &data_key, const std::filesystem::path &destination);
  static VxCoreError FingerprintObject(const std::filesystem::path &path,
                                       Fingerprint &out_fingerprint);
  static VxCoreError FingerprintBytes(const void *bytes, size_t size, Fingerprint &out_fingerprint);

  // The single note/backup payload codec, also used by create/conversion staging.
  // Writer publishes ciphertext atomically; caller supplies an NK lease for the
  // complete operation and performs transaction/containment/metadata preflight.
  // A nonnegative backup_revision selects kind=backup and an authenticated LE32
  // revision prefix before the otherwise identical note payload. -1 selects note.
  static VxCoreError ValidateNoteManifest(const nlohmann::json &manifest);
  static VxCoreError WriteNoteSnapshot(
      const std::filesystem::path &path, const KeyEnvelope &envelope, const Key &notebook_key,
      const Key &note_key, const std::string &document_id, const std::vector<uint8_t> &body,
      const nlohmann::json &manifest, ObjectHeader *out_header = nullptr,
      Fingerprint *out_fingerprint = nullptr, int backup_revision = -1);
  // Expected header must first be parsed and its DK unwrapped. All outputs remain
  // unchanged on failure. Manifest and body owners must be wiped by their caller.
  static VxCoreError ReadNoteSnapshot(
      const std::filesystem::path &path, const ObjectHeader &expected, const Key &note_key,
      std::vector<uint8_t> &out_body, nlohmann::json &out_manifest,
      Fingerprint *out_fingerprint = nullptr, int *out_revision = nullptr);

  // Transfer staging contains ciphertext only. Copy generates an independent
  // document/DK and immutable objects; Move authenticates and copies the payload
  // unchanged, replacing only its NK envelope. Caller owns publication/recovery,
  // safe path checks, and both notebook-key leases through commit/discard.
  static VxCoreError TransferNote(
      const std::filesystem::path &source, const std::filesystem::path &source_assets,
      const KeyEnvelope &source_envelope, const Key &source_notebook_key,
      const std::filesystem::path &destination, const std::filesystem::path &destination_assets,
      const KeyEnvelope &destination_envelope, const Key &destination_notebook_key,
      bool copy, const std::filesystem::path &backup_destination, bool &out_has_backup);

  // Install only after authenticated preparation/unlock AND successful publication.
  // The caller serializes owner creation/teardown just like other Notebook operations,
  // and prevents new installs/leases during locking. A derived DK does not implicitly
  // lease its parent: protected buffers/jobs retain an NK lease through their lifetime.
  VxCoreError InstallNotebookKey(const KeyEnvelope &envelope, Key &&notebook_key);
  VxCoreError AcquireNotebookKey(std::shared_ptr<const Key> &out_key,
                                KeyEnvelope *out_envelope = nullptr) const;
  VxCoreError LockNotebookKey();

 private:
  friend class NotebookManager;
  // Registry callers compare the authenticated password envelope first. This
  // authenticates just NK under an already-unlocked MK and never runs a KDF.
  static VxCoreError UnlockNotebookKey(const KeyEnvelope &envelope, const Key &master_key,
                                       Key &out_notebook_key);
  static VxCoreError AllocateKey(Key &out_key);
  static VxCoreError DeriveKey(const void *password, size_t password_size,
                               const std::array<unsigned char, 16> &salt, Key &out_key);
  static VxCoreError WrapKey(const Key &wrapping_key, const Key &key, const std::string &aad,
                             WrappedKey &out_wrapped);
  static VxCoreError UnwrapKey(const Key &wrapping_key, const WrappedKey &wrapped,
                               const std::string &aad, Key &out_key);
  using PlaintextSink = std::function<VxCoreError(const unsigned char *, size_t)>;
  // Only called with private transactional sinks, never a consumer callback.
  static VxCoreError DecryptRecords(std::istream &input, const ObjectHeader &header,
                                    const Key &data_key, const PlaintextSink &sink,
                                    const PlaintextSink &ciphertext_sink = {});
  static VxCoreError TransferObject(
      const std::filesystem::path &source, const ObjectHeader &expected, const Key &source_key,
      const std::filesystem::path &destination, const ObjectHeader &replacement,
      const Key &destination_key, bool reencrypt);

  mutable std::mutex mutex_;
  KeyEnvelope envelope_;
  std::shared_ptr<const Key> notebook_key_;
  bool locking_ = false;
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

  // Cached runtime recovery interlock, independent of the user's read-only setting.
  // Callers serialize changes with the notebook maintenance/IO lease.
  VxCoreError CheckWritable() const noexcept {
    return encryption_recovery_required_ ? VXCORE_ERR_ENCRYPTION_RECOVERY_REQUIRED
                                         : (read_only_ ? VXCORE_ERR_READ_ONLY : VXCORE_OK);
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
  // Immediate encrypted manifest commits use the same durable-file mutation fact as
  // BufferManager body saves, without putting resource display names into metadata.
  void NotifyEncryptedManifestSaved(const std::string &file_path) noexcept;

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
  // The sparse session registry retains this owner across Close(), so live key
  // leases cannot disappear from Lock All preflight when a notebook is closed.
  std::shared_ptr<NotebookEncryption> encryption_;

  static const char *kConfigFileName;
};

}  // namespace vxcore

#endif
