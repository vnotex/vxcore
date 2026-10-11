#ifndef VXCORE_ENCRYPTEDNOTE_H
#define VXCORE_ENCRYPTEDNOTE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <istream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "vxcore/vxcore_types.h"

namespace vxcore {

// Private-core authenticated storage. None of these owners exist on plaintext paths.
class EncryptedNote final {
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
    friend class EncryptedNote;
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
    friend class EncryptedNote;
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
    int version = 1;
    KeyEnvelope envelope;  // Complete wrapped hierarchy, present only in version 2.
    std::string kind;
    std::string document_id;
    std::string object_id;
    // Every supported object is a note or backup carrying its wrapped document key.
    std::string notebook_key_id;
    WrappedKey note_key;
  };
  using Fingerprint = std::array<unsigned char, 32>;

  EncryptedNote() = delete;

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
  // All failures clear out_plaintext. No plaintext is returned before FINAL and EOF.
  static VxCoreError ReadObject(const std::filesystem::path &path, const ObjectHeader &expected,
                                const Key &data_key, size_t max_bytes, SecureBytes &out_plaintext,
                                Fingerprint *out_fingerprint = nullptr);
  // Conversion staging verification authenticates FINAL/EOF with bounded memory.
  static VxCoreError VerifyObject(const std::filesystem::path &path,
                                  const ObjectHeader &expected, const Key &data_key);
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

  // Transfer owns only note/backup bodies and key envelopes, never asset files.
  // A caller may rewrite links using the ordinary content handler; true indicates
  // a changed body. Unchanged moves preserve payload bytes and only rewrap the DK.
  // Caller owns publication/recovery and both notebook-key leases through commit.
  using BodyTransform = std::function<bool(std::vector<uint8_t> &, const std::string &)>;
  static VxCoreError TransferNote(
      const std::filesystem::path &source, const KeyEnvelope &source_envelope,
      const Key &source_notebook_key, const std::filesystem::path &destination,
      const KeyEnvelope &destination_envelope, const Key &destination_notebook_key,
      bool copy, const std::filesystem::path &backup_destination, bool &out_has_backup,
      const BodyTransform &transform_body);

  static bool SameEnvelope(const KeyEnvelope &left, const KeyEnvelope &right) noexcept;
  static bool SameObject(const ObjectHeader &left, const ObjectHeader &right) noexcept;

  // Header-only upgrade: payload identity/AAD and encrypted stream are unchanged.
  static VxCoreError MakeSelfContained(ObjectHeader &header, const KeyEnvelope &envelope,
                                      const Key &notebook_key, const Key &note_key);
  // Explicit writable-open migration. changed remains true after partial publication.
  static VxCoreError MigrateNote(const std::filesystem::path &path,
                                const KeyEnvelope &envelope, const Key &notebook_key,
                                bool &changed);

  // Registry callers compare the authenticated password envelope first. This
  // authenticates just NK under an already-unlocked MK and never runs a KDF.
  static VxCoreError UnlockNotebookKey(const KeyEnvelope &envelope, const Key &master_key,
                                       Key &out_notebook_key);
 private:
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
  static VxCoreError ReadNoteSnapshotImpl(
      const std::filesystem::path &path, const ObjectHeader &expected, const Key &note_key,
      std::vector<uint8_t> *out_body, nlohmann::json &out_manifest,
      Fingerprint *out_fingerprint, int *out_revision);
  static VxCoreError RewrapSnapshot(
      const std::filesystem::path &source, const ObjectHeader &expected, const Key &note_key,
      const std::filesystem::path &destination, const ObjectHeader &replacement,
      const Fingerprint *expected_fingerprint = nullptr,
      const std::function<VxCoreError()> &before_commit = {},
      Fingerprint *out_fingerprint = nullptr);

};

}  // namespace vxcore

#endif  // VXCORE_ENCRYPTEDNOTE_H
