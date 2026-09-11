#include "notebook.h"

#include <vxcore/notebook_json_keys.h>
#include <vxcore/vxcore_types.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <set>
#include <sodium.h>
#include <streambuf>
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


namespace {

constexpr unsigned char kKeyMagic[8] = {'V', 'N', 'E', 'K', 'E', 'Y', '1', 0};
constexpr unsigned char kObjectMagic[8] = {'V', 'N', 'O', 'T', 'E', 'E', '1', 0};
constexpr uint64_t kKdfOps = 3;
constexpr uint64_t kKdfMemory = 67108864;
constexpr size_t kStreamOverhead = crypto_secretstream_xchacha20poly1305_ABYTES;
using Encryption = NotebookEncryption;
using Json = nlohmann::json;
struct EncryptionFormatError {};

static_assert(crypto_aead_xchacha20poly1305_ietf_KEYBYTES == Encryption::kKeyBytes);
static_assert(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES == 24);
static_assert(crypto_aead_xchacha20poly1305_ietf_ABYTES == 16);
static_assert(crypto_pwhash_SALTBYTES == 16);
static_assert(crypto_secretstream_xchacha20poly1305_KEYBYTES == Encryption::kKeyBytes);
static_assert(crypto_secretstream_xchacha20poly1305_HEADERBYTES == 24);
static_assert(kStreamOverhead == 17);

VxCoreError EnsureSodium() {
  // Function-local initialization runs exactly once, only at the first crypto operation.
  static const int initialized = sodium_init();
  return initialized < 0 ? VXCORE_ERR_NOT_INITIALIZED : VXCORE_OK;
}

template <typename Function>
VxCoreError CryptoResult(Function &&function) {
  try {
    return function();
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::length_error &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const EncryptionFormatError &) {
    return VXCORE_ERR_ENCRYPTION_FORMAT;
  } catch (const Json::exception &) {
    return VXCORE_ERR_ENCRYPTION_FORMAT;
  } catch (const std::ios_base::failure &) {
    return VXCORE_ERR_IO;
  } catch (const std::filesystem::filesystem_error &) {
    return VXCORE_ERR_IO;
  }
}

void Require(bool condition) {
  if (!condition) {
    throw EncryptionFormatError{};
  }
}

// Fixed-format identities use randombytes rather than the ordinary UUID helper's
// shared PRNG: protected preparations may run concurrently on KDF workers.
std::string CryptoUuid() {
  std::array<unsigned char, 16> bytes{};
  randombytes_buf(bytes.data(), bytes.size());
  bytes[6] = static_cast<unsigned char>((bytes[6] & 15) | 64);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 63) | 128);
  std::array<char, 33> hex{};
  sodium_bin2hex(hex.data(), hex.size(), bytes.data(), bytes.size());
  std::string result;
  result.reserve(36);
  for (size_t index = 0; index < 32; ++index) {
    if (index == 8 || index == 12 || index == 16 || index == 20) {
      result.push_back('-');
    }
    result.push_back(hex[index]);
  }
  return result;
}

template <size_t Size>
std::string Base64(const std::array<unsigned char, Size> &bytes) {
  std::array<char, sodium_base64_ENCODED_LEN(Size, sodium_base64_VARIANT_ORIGINAL)> encoded{};
  sodium_bin2base64(encoded.data(), encoded.size(), bytes.data(), bytes.size(),
                    sodium_base64_VARIANT_ORIGINAL);
  return std::string(encoded.data());
}

template <size_t Size>
void DecodeBase64(const Json &value, std::array<unsigned char, Size> &out_bytes) {
  Require(value.is_string());
  const auto &text = value.get_ref<const std::string &>();
  Require(text.size() == sodium_base64_ENCODED_LEN(Size, sodium_base64_VARIANT_ORIGINAL) - 1);
  size_t decoded_size = 0;
  const char *end = nullptr;
  Require(sodium_base642bin(out_bytes.data(), out_bytes.size(), text.data(), text.size(), nullptr,
                           &decoded_size, &end, sodium_base64_VARIANT_ORIGINAL) == 0);
  Require(decoded_size == Size && end == text.data() + text.size());
  Require(Base64(out_bytes) == text);  // Also rejects noncanonical unused bits/padding.
}

Json ParseFlatHeader(const unsigned char *bytes, size_t size) {
  Require(size != 0 && size <= Encryption::kMaxHeaderBytes);
  std::set<std::string> keys;
  auto callback = [&keys](int depth, Json::parse_event_t event, Json &value) {
    if (event == Json::parse_event_t::object_start) {
      Require(depth == 0);
    } else if (event == Json::parse_event_t::array_start) {
      throw EncryptionFormatError{};
    } else if (event == Json::parse_event_t::key) {
      Require(keys.insert(value.get<std::string>()).second);
    }
    return true;
  };
  auto result = Json::parse(bytes, bytes + size, callback);
  Require(result.is_object());
  return result;
}

void ExactKeys(const Json &json, std::initializer_list<const char *> keys) {
  Require(json.is_object() && json.size() == keys.size());
  for (const auto *key : keys) {
    Require(json.contains(key));
  }
}

bool IsInteger(const Json &json, uint64_t expected) {
  if (json.is_number_unsigned()) {
    return json.get<uint64_t>() == expected;
  }
  return json.is_number_integer() && json.get<int64_t>() >= 0 &&
         static_cast<uint64_t>(json.get<int64_t>()) == expected;
}

std::string UuidField(const Json &json, const char *key) {
  Require(json.at(key).is_string());
  auto value = json.at(key).get<std::string>();
  Require(Encryption::IsCanonicalUuid(value));
  return value;
}

void ValidateEnvelope(const Encryption::KeyEnvelope &envelope) {
  Require(Encryption::IsCanonicalUuid(envelope.vault_id));
  Require(Encryption::IsCanonicalUuid(envelope.notebook_id));
  Require(Encryption::IsCanonicalUuid(envelope.notebook_key_id));
}

std::string MasterAad(const Encryption::KeyEnvelope &envelope) {
  return Json::array({"vnote-master", 1, envelope.vault_id, "argon2id13", kKdfOps, kKdfMemory,
                      Base64(envelope.salt)}).dump();
}

std::string NotebookAad(const Encryption::KeyEnvelope &envelope) {
  return Json::array({"vnote-notebook", 1, envelope.vault_id, envelope.notebook_id,
                      envelope.notebook_key_id}).dump();
}

bool CarriesNoteKey(const Encryption::ObjectHeader &header) {
  return header.kind == "note" || header.kind == "backup";
}

void ValidateObject(const Encryption::ObjectHeader &header) {
  Require(CarriesNoteKey(header));
  Require(Encryption::IsCanonicalUuid(header.document_id));
  Require(Encryption::IsCanonicalUuid(header.object_id));
  Require(Encryption::IsCanonicalUuid(header.notebook_key_id));
}

std::string ObjectAad(const Encryption::ObjectHeader &header) {
  return Json::array({"vnote-object", 1, header.kind, header.document_id, header.object_id}).dump();
}

std::string NoteKeyAad(const std::string &notebook_id, const Encryption::ObjectHeader &header) {
  return Json::array({"vnote-note-key", 1, notebook_id, header.notebook_key_id, header.document_id,
                      header.kind, header.object_id}).dump();
}

std::string SerializeObject(const Encryption::ObjectHeader &header) {
  ValidateObject(header);
  Json json = {{kJsonKeyVersion, 1}, {kJsonKeyKind, header.kind}, {kJsonKeyDocumentId, header.document_id},
               {kJsonKeyObjectId, header.object_id}};
  json[kJsonKeyNotebookKeyId] = header.notebook_key_id;
  json[kJsonKeyKeyNonce] = Base64(header.note_key.nonce);
  json[kJsonKeyWrappedNoteKey] = Base64(header.note_key.ciphertext);
  auto text = json.dump();
  Require(text.size() <= Encryption::kMaxHeaderBytes);
  return text;
}

Encryption::ObjectHeader ParseObject(const unsigned char *bytes, size_t size) {
  auto json = ParseFlatHeader(bytes, size);
  Require(json.contains(kJsonKeyKind) && json[kJsonKeyKind].is_string());
  Encryption::ObjectHeader header;
  header.kind = json[kJsonKeyKind].get<std::string>();
  Require(CarriesNoteKey(header));
  ExactKeys(json, {kJsonKeyVersion, kJsonKeyKind, kJsonKeyDocumentId, kJsonKeyObjectId,
                   kJsonKeyNotebookKeyId, kJsonKeyKeyNonce, kJsonKeyWrappedNoteKey});
  header.notebook_key_id = UuidField(json, kJsonKeyNotebookKeyId);
  DecodeBase64(json[kJsonKeyKeyNonce], header.note_key.nonce);
  DecodeBase64(json[kJsonKeyWrappedNoteKey], header.note_key.ciphertext);
  Require(IsInteger(json[kJsonKeyVersion], 1));
  header.document_id = UuidField(json, kJsonKeyDocumentId);
  header.object_id = UuidField(json, kJsonKeyObjectId);
  ValidateObject(header);
  return header;
}

bool SameObject(const Encryption::ObjectHeader &left, const Encryption::ObjectHeader &right) {
  return left.kind == right.kind && left.document_id == right.document_id &&
         left.object_id == right.object_id && left.notebook_key_id == right.notebook_key_id &&
         left.note_key.nonce == right.note_key.nonce &&
         left.note_key.ciphertext == right.note_key.ciphertext;
}

uint32_t ReadLittle32(const unsigned char *bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

std::array<unsigned char, 4> Little32(uint32_t value) {
  return {static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8),
          static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 24)};
}

VxCoreError ReadExact(std::istream &input, void *bytes, size_t size) {
  input.read(static_cast<char *>(bytes), static_cast<std::streamsize>(size));
  if (input.bad()) {
    return VXCORE_ERR_IO;
  }
  return input.gcount() == static_cast<std::streamsize>(size) ? VXCORE_OK
                                                            : VXCORE_ERR_ENCRYPTION_FORMAT;
}

VxCoreError RequireEnd(std::istream &input) {
  const auto next = input.peek();
  if (input.bad() || (input.fail() && !input.eof())) {
    return VXCORE_ERR_IO;
  }
  return next == std::char_traits<char>::eof() ? VXCORE_OK : VXCORE_ERR_ENCRYPTION_FORMAT;
}

VxCoreError ReadStorageHeader(std::istream &input, const unsigned char (&magic)[8],
                             std::array<unsigned char, Encryption::kMaxHeaderBytes> &bytes,
                             uint32_t &size) {
  std::array<unsigned char, 12> prefix{};
  auto error = ReadExact(input, prefix.data(), prefix.size());
  if (error != VXCORE_OK) {
    return error;
  }
  Require(std::equal(std::begin(magic), std::end(magic), prefix.begin()));
  size = ReadLittle32(prefix.data() + 8);
  Require(size != 0 && size <= bytes.size());
  return ReadExact(input, bytes.data(), size);
}

VxCoreError WriteStorageHeader(AtomicFileWriter &writer, const unsigned char (&magic)[8],
                              const std::string &header) {
  auto error = writer.Write(magic, sizeof(magic));
  if (error == VXCORE_OK) {
    const auto size = Little32(static_cast<uint32_t>(header.size()));
    error = writer.Write(size.data(), size.size());
  }
  return error == VXCORE_OK ? writer.Write(header.data(), header.size()) : error;
}

// Stream state contains a derived key; guarded allocation also handles every early return.
struct StreamState final {
  crypto_secretstream_xchacha20poly1305_state *state = nullptr;
  StreamState()
      : state(static_cast<crypto_secretstream_xchacha20poly1305_state *>(
            sodium_malloc(sizeof(crypto_secretstream_xchacha20poly1305_state)))) {}
  ~StreamState() {
    if (state) {
      sodium_free(state);
    }
  }
  StreamState(const StreamState &) = delete;
  StreamState &operator=(const StreamState &) = delete;
};

struct SecretString final {
  std::string value;
  ~SecretString() { Encryption::WipeString(value); }
};

struct SecretJson final {
  Json value;
  ~SecretJson() { Encryption::WipeJson(value); }
};

struct SecretVector final {
  std::vector<uint8_t> value;
  ~SecretVector() { Encryption::WipeBytes(value); }
};

// Borrowed segments are consumed directly by the existing record writer. In
// particular, framing a note never constructs another full-body string.
class SegmentedInput final : public std::streambuf {
 public:
  struct Segment {
    const void *data;
    size_t size;
  };
  SegmentedInput(const Segment *segments, size_t count) : segments_(segments), count_(count) {}

 protected:
  int_type underflow() override {
    while (next_ < count_) {
      const auto &segment = segments_[next_++];
      if (segment.size) {
        auto *begin = const_cast<char *>(static_cast<const char *>(segment.data));
        setg(begin, begin, begin + segment.size);
        return traits_type::to_int_type(*gptr());
      }
    }
    return traits_type::eof();
  }

 private:
  const Segment *segments_;
  size_t count_;
  size_t next_ = 0;
};

void HashStorageHeader(crypto_hash_sha256_state &state, const unsigned char *bytes, size_t size) {
  const auto length = Little32(static_cast<uint32_t>(size));
  crypto_hash_sha256_update(&state, kObjectMagic, sizeof(kObjectMagic));
  crypto_hash_sha256_update(&state, length.data(), length.size());
  crypto_hash_sha256_update(&state, bytes, size);
}


void ValidateManifest(const Json &manifest) {
  // Only an empty legacy resources field is compatible. Reject old encrypted-asset
  // snapshots explicitly rather than silently dropping their referenced ciphertext.
  if (manifest.contains(kJsonKeyResources)) {
    ExactKeys(manifest, {kJsonKeyEditorType, kJsonKeyResources});
    Require(manifest.at(kJsonKeyResources).is_array() && manifest.at(kJsonKeyResources).empty());
  } else {
    ExactKeys(manifest, {kJsonKeyEditorType});
  }
  const auto &editor = manifest.at(kJsonKeyEditorType);
  Require(editor.is_string() &&
          (editor == "markdown" || editor == "text" || editor == "mindmap"));
}

Json ParseManifest(const unsigned char *bytes, size_t size) {
  // Reject duplicate keys before JSON parsing can silently replace a field.
  std::set<std::string> keys;
  auto callback = [&keys](int depth, Json::parse_event_t event, Json &value) {
    if (event == Json::parse_event_t::object_start) {
      Require(depth == 0);
    } else if (event == Json::parse_event_t::array_start) {
      Require(depth == 1);
    } else if (event == Json::parse_event_t::key) {
      Require(depth == 1 && keys.insert(value.get<std::string>()).second);
    }
    return true;
  };
  SecretJson parsed;
  parsed.value = Json::parse(bytes, bytes + size, callback);
  ValidateManifest(parsed.value);
  parsed.value.erase(kJsonKeyResources);
  return std::move(parsed.value);
}

}  // namespace

NotebookEncryption::Key::~Key() { Reset(); }

NotebookEncryption::Key::Key(Key &&other) noexcept : data_(std::exchange(other.data_, nullptr)) {}

NotebookEncryption::Key &NotebookEncryption::Key::operator=(Key &&other) noexcept {
  if (this != &other) {
    Reset();
    data_ = std::exchange(other.data_, nullptr);
  }
  return *this;
}

void NotebookEncryption::Key::Reset() noexcept {
  if (data_) {
    sodium_free(data_);  // Wipes the entire guarded allocation before unlocking/freeing it.
    data_ = nullptr;
  }
}

NotebookEncryption::SecureBytes::~SecureBytes() { Reset(); }

NotebookEncryption::SecureBytes::SecureBytes(SecureBytes &&other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
      capacity_(std::exchange(other.capacity_, 0)) {}

NotebookEncryption::SecureBytes &NotebookEncryption::SecureBytes::operator=(SecureBytes &&other) noexcept {
  if (this != &other) {
    Reset();
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
    capacity_ = std::exchange(other.capacity_, 0);
  }
  return *this;
}

void NotebookEncryption::SecureBytes::Reset() noexcept {
  if (data_) {
    sodium_free(data_);  // sodium_free wipes capacity, including unauthenticated/unused bytes.
    data_ = nullptr;
  }
  size_ = capacity_ = 0;
}

VxCoreError NotebookEncryption::SecureBytes::Allocate(size_t capacity) {
  auto error = EnsureSodium();
  if (error != VXCORE_OK) {
    return error;
  }
  auto *data = static_cast<unsigned char *>(sodium_malloc(capacity));
  if (!data) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  }
  Reset();
  data_ = data;
  capacity_ = capacity;
  return VXCORE_OK;
}

bool NotebookEncryption::IsCanonicalUuid(const std::string &value) noexcept {
  if (value.size() != 36) {
    return false;
  }
  for (size_t index = 0; index < value.size(); ++index) {
    const auto character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
    } else if (!((character >= '0' && character <= '9') ||
                 (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

VxCoreError NotebookEncryption::AllocateKey(Key &out_key) {
  auto error = EnsureSodium();
  if (error != VXCORE_OK) {
    return error;
  }
  Key key;
  key.data_ = static_cast<unsigned char *>(sodium_malloc(kKeyBytes));
  if (!key.data_) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  }
  out_key = std::move(key);
  return VXCORE_OK;
}

VxCoreError NotebookEncryption::GenerateKey(Key &out_key) {
  Key key;
  auto error = AllocateKey(key);
  if (error == VXCORE_OK) {
    randombytes_buf(key.data_, kKeyBytes);
    out_key = std::move(key);
  }
  return error;
}

bool NotebookEncryption::KeysEqual(const Key &left, const Key &right) noexcept {
  return left.IsValid() && right.IsValid() &&
         sodium_memcmp(left.data_, right.data_, kKeyBytes) == 0;
}

VxCoreError NotebookEncryption::DeriveKey(const void *password, size_t password_size,
                                         const std::array<unsigned char, 16> &salt, Key &out_key) {
  if (!password && password_size) {
    return VXCORE_ERR_NULL_POINTER;
  }
  if (password_size > crypto_pwhash_PASSWD_MAX) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  SecureBytes password_copy;
  auto error = password_copy.Allocate(password_size);
  if (error != VXCORE_OK) {
    return error;
  }
  if (password_size) {
    std::memcpy(password_copy.data_, password, password_size);
  }
  Key key;
  error = AllocateKey(key);
  if (error != VXCORE_OK) {
    return error;
  }
  const int result = crypto_pwhash(key.data_, kKeyBytes,
                                   reinterpret_cast<const char *>(password_copy.data_), password_size,
                                   salt.data(), kKdfOps, static_cast<size_t>(kKdfMemory),
                                   crypto_pwhash_ALG_ARGON2ID13);
  password_copy.Reset();
  if (result != 0) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  }
  out_key = std::move(key);
  return VXCORE_OK;
}

VxCoreError NotebookEncryption::WrapKey(const Key &wrapping_key, const Key &key,
                                       const std::string &aad, WrappedKey &out_wrapped) {
  if (!wrapping_key.IsValid() || !key.IsValid()) {
    return VXCORE_ERR_ENCRYPTION_LOCKED;
  }
  WrappedKey wrapped;
  randombytes_buf(wrapped.nonce.data(), wrapped.nonce.size());
  unsigned long long size = 0;
  if (crypto_aead_xchacha20poly1305_ietf_encrypt(
          wrapped.ciphertext.data(), &size, key.data_, kKeyBytes,
          reinterpret_cast<const unsigned char *>(aad.data()), aad.size(), nullptr,
          wrapped.nonce.data(), wrapping_key.data_) != 0 || size != wrapped.ciphertext.size()) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }
  out_wrapped = wrapped;
  return VXCORE_OK;
}

VxCoreError NotebookEncryption::UnwrapKey(const Key &wrapping_key, const WrappedKey &wrapped,
                                         const std::string &aad, Key &out_key) {
  if (!wrapping_key.IsValid()) {
    return VXCORE_ERR_ENCRYPTION_LOCKED;
  }
  Key key;
  auto error = AllocateKey(key);
  if (error != VXCORE_OK) {
    return error;
  }
  unsigned long long size = 0;
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(
          key.data_, &size, nullptr, wrapped.ciphertext.data(), wrapped.ciphertext.size(),
          reinterpret_cast<const unsigned char *>(aad.data()), aad.size(), wrapped.nonce.data(),
          wrapping_key.data_) != 0 || size != kKeyBytes) {
    return VXCORE_ERR_ENCRYPTION_AUTH_FAILED;
  }
  out_key = std::move(key);
  return VXCORE_OK;
}

VxCoreError NotebookEncryption::PrepareNewKeys(const std::string &notebook_id, const void *password,
                                              size_t password_size, KeyEnvelope &out_envelope,
                                              Key &out_master_key, Key &out_notebook_key) {
  return CryptoResult([&]() -> VxCoreError {
    if (!IsCanonicalUuid(notebook_id) || &out_master_key == &out_notebook_key ||
        password_size == 0) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    auto error = EnsureSodium();
    if (error != VXCORE_OK) {
      return error;
    }
    KeyEnvelope envelope;
    envelope.vault_id = CryptoUuid();
    envelope.notebook_id = notebook_id;
    envelope.notebook_key_id = CryptoUuid();
    randombytes_buf(envelope.salt.data(), envelope.salt.size());
    Key kek, master_key, notebook_key;
    error = DeriveKey(password, password_size, envelope.salt, kek);
    if (error == VXCORE_OK) {
      error = GenerateKey(master_key);
    }
    if (error == VXCORE_OK) {
      error = WrapKey(kek, master_key, MasterAad(envelope), envelope.master_key);
    }
    kek.Reset();
    if (error == VXCORE_OK) {
      error = GenerateKey(notebook_key);
    }
    if (error == VXCORE_OK) {
      error = WrapKey(master_key, notebook_key, NotebookAad(envelope), envelope.notebook_key);
    }
    if (error == VXCORE_OK) {
      out_envelope = std::move(envelope);
      out_master_key = std::move(master_key);
      out_notebook_key = std::move(notebook_key);
    }
    return error;
  });
}

VxCoreError NotebookEncryption::PrepareNotebookKeys(const KeyEnvelope &authenticated_source,
                                                   const Key &master_key,
                                                   const std::string &notebook_id,
                                                   KeyEnvelope &out_envelope, Key &out_notebook_key) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateEnvelope(authenticated_source);
    if (!IsCanonicalUuid(notebook_id) || notebook_id == authenticated_source.notebook_id ||
        &master_key == &out_notebook_key) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    // Verify the provided master against the source's authenticated notebook envelope.
    Key source_key;
    auto error = UnlockNotebookKey(authenticated_source, master_key, source_key);
    if (error != VXCORE_OK) {
      return error;
    }
    source_key.Reset();
    KeyEnvelope envelope = authenticated_source;
    envelope.notebook_id = notebook_id;
    envelope.notebook_key_id = CryptoUuid();
    Key notebook_key;
    error = GenerateKey(notebook_key);
    if (error == VXCORE_OK) {
      error = WrapKey(master_key, notebook_key, NotebookAad(envelope), envelope.notebook_key);
    }
    if (error == VXCORE_OK) {
      out_envelope = std::move(envelope);
      out_notebook_key = std::move(notebook_key);
    }
    return error;
  });
}

VxCoreError NotebookEncryption::UnlockKeys(const KeyEnvelope &envelope, const std::string &notebook_id,
                                          const void *password, size_t password_size,
                                          Key &out_master_key, Key &out_notebook_key) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateEnvelope(envelope);
    if (&out_master_key == &out_notebook_key) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    if (notebook_id != envelope.notebook_id) {
      return VXCORE_ERR_ENCRYPTION_FORMAT;
    }
    Key kek, master_key, notebook_key;
    auto error = DeriveKey(password, password_size, envelope.salt, kek);
    if (error == VXCORE_OK) {
      error = UnwrapKey(kek, envelope.master_key, MasterAad(envelope), master_key);
    }
    kek.Reset();
    if (error == VXCORE_OK) {
      error = UnlockNotebookKey(envelope, master_key, notebook_key);
    }
    if (error == VXCORE_OK) {
      out_master_key = std::move(master_key);
      out_notebook_key = std::move(notebook_key);
    }
    return error;
  });
}

VxCoreError NotebookEncryption::UnlockNotebookKey(const KeyEnvelope &envelope,
                                                   const Key &master_key, Key &out_notebook_key) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateEnvelope(envelope);
    if (&master_key == &out_notebook_key) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    return UnwrapKey(master_key, envelope.notebook_key, NotebookAad(envelope), out_notebook_key);
  });
}

VxCoreError NotebookEncryption::EncodeKeyEnvelope(const KeyEnvelope &envelope,
                                                  std::string &out_bytes) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateEnvelope(envelope);
    Json json = {{kJsonKeyVersion, 1}, {kJsonKeyVaultId, envelope.vault_id},
                 {kJsonKeyNotebookId, envelope.notebook_id},
                 {kJsonKeyNotebookKeyId, envelope.notebook_key_id}, {kJsonKeyKdf, "argon2id13"},
                 {kJsonKeyOpslimit, kKdfOps}, {kJsonKeyMemlimit, kKdfMemory}, {kJsonKeySalt, Base64(envelope.salt)},
                 {kJsonKeyMasterNonce, Base64(envelope.master_key.nonce)},
                 {kJsonKeyWrappedMasterKey, Base64(envelope.master_key.ciphertext)},
                 {kJsonKeyNotebookNonce, Base64(envelope.notebook_key.nonce)},
                 {kJsonKeyWrappedNotebookKey, Base64(envelope.notebook_key.ciphertext)}};
    const auto header = json.dump();
    Require(header.size() <= kMaxHeaderBytes - 12);
    std::string bytes(reinterpret_cast<const char *>(kKeyMagic), sizeof(kKeyMagic));
    const auto size = Little32(static_cast<uint32_t>(header.size()));
    bytes.append(reinterpret_cast<const char *>(size.data()), size.size());
    bytes.append(header);
    out_bytes = std::move(bytes);
    return VXCORE_OK;
  });
}

VxCoreError NotebookEncryption::DecodeKeyEnvelope(const void *bytes, size_t size,
                                                  KeyEnvelope &out_envelope) {
  return CryptoResult([&]() -> VxCoreError {
    if (!bytes && size) {
      return VXCORE_ERR_NULL_POINTER;
    }
    Require(size > 12 && size <= kMaxHeaderBytes);
    const auto *data = static_cast<const unsigned char *>(bytes);
    Require(std::equal(std::begin(kKeyMagic), std::end(kKeyMagic), data));
    const auto header_size = ReadLittle32(data + 8);
    Require(header_size == size - 12);
    auto json = ParseFlatHeader(data + 12, header_size);
    ExactKeys(json, {kJsonKeyVersion, kJsonKeyVaultId, kJsonKeyNotebookId, kJsonKeyNotebookKeyId, kJsonKeyKdf, kJsonKeyOpslimit,
                     kJsonKeyMemlimit, kJsonKeySalt, kJsonKeyMasterNonce, kJsonKeyWrappedMasterKey, kJsonKeyNotebookNonce,
                     kJsonKeyWrappedNotebookKey});
    Require(IsInteger(json[kJsonKeyVersion], 1));
    Require(json[kJsonKeyKdf].is_string() && json[kJsonKeyKdf] == "argon2id13");
    Require(IsInteger(json[kJsonKeyOpslimit], kKdfOps) && IsInteger(json[kJsonKeyMemlimit], kKdfMemory));
    KeyEnvelope envelope;
    envelope.vault_id = UuidField(json, kJsonKeyVaultId);
    envelope.notebook_id = UuidField(json, kJsonKeyNotebookId);
    envelope.notebook_key_id = UuidField(json, kJsonKeyNotebookKeyId);
    DecodeBase64(json[kJsonKeySalt], envelope.salt);
    DecodeBase64(json[kJsonKeyMasterNonce], envelope.master_key.nonce);
    DecodeBase64(json[kJsonKeyWrappedMasterKey], envelope.master_key.ciphertext);
    DecodeBase64(json[kJsonKeyNotebookNonce], envelope.notebook_key.nonce);
    DecodeBase64(json[kJsonKeyWrappedNotebookKey], envelope.notebook_key.ciphertext);
    out_envelope = std::move(envelope);
    return VXCORE_OK;
  });
}

VxCoreError NotebookEncryption::ReadKeyEnvelope(const std::filesystem::path &path,
                                                KeyEnvelope &out_envelope) {
  return CryptoResult([&]() -> VxCoreError {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      return VXCORE_ERR_IO;
    }
    std::array<unsigned char, kMaxHeaderBytes> bytes{};
    auto error = ReadExact(input, bytes.data(), 12);
    if (error != VXCORE_OK) {
      return error;
    }
    const auto size = ReadLittle32(bytes.data() + 8);
    Require(size != 0 && size <= bytes.size() - 12);
    error = ReadExact(input, bytes.data() + 12, size);
    if (error == VXCORE_OK) {
      error = RequireEnd(input);
    }
    return error == VXCORE_OK ? DecodeKeyEnvelope(bytes.data(), size + 12, out_envelope) : error;
  });
}

VxCoreError NotebookEncryption::WrapNoteKey(const std::string &notebook_id,
                                           const std::string &notebook_key_id,
                                           const Key &notebook_key, const Key &note_key,
                                           ObjectHeader &in_out_header) {
  return CryptoResult([&]() -> VxCoreError {
    if (!IsCanonicalUuid(notebook_id) || !IsCanonicalUuid(notebook_key_id) ||
        !CarriesNoteKey(in_out_header)) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    auto header = in_out_header;
    header.notebook_key_id = notebook_key_id;
    ValidateObject(header);
    auto error = WrapKey(notebook_key, note_key, NoteKeyAad(notebook_id, header), header.note_key);
    if (error == VXCORE_OK) {
      in_out_header = std::move(header);
    }
    return error;
  });
}

VxCoreError NotebookEncryption::UnwrapNoteKey(const std::string &notebook_id,
                                             const std::string &notebook_key_id,
                                             const Key &notebook_key, const ObjectHeader &header,
                                             Key &out_note_key) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateObject(header);
    if (!CarriesNoteKey(header) || !IsCanonicalUuid(notebook_id) ||
        notebook_key_id != header.notebook_key_id) {
      return VXCORE_ERR_ENCRYPTION_FORMAT;
    }
    if (&notebook_key == &out_note_key) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    return UnwrapKey(notebook_key, header.note_key, NoteKeyAad(notebook_id, header), out_note_key);
  });
}

VxCoreError NotebookEncryption::ReadObjectHeader(const std::filesystem::path &path,
                                                 ObjectHeader &out_header) {
  return CryptoResult([&]() -> VxCoreError {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      return VXCORE_ERR_IO;
    }
    std::array<unsigned char, kMaxHeaderBytes> bytes{};
    uint32_t size = 0;
    auto error = ReadStorageHeader(input, kObjectMagic, bytes, size);
    if (error == VXCORE_OK) {
      out_header = ParseObject(bytes.data(), size);
    }
    return error;
  });
}

VxCoreError NotebookEncryption::EncryptObject(const std::filesystem::path &path,
                                              const ObjectHeader &header, const Key &data_key,
                                              std::istream &plaintext,
                                              Fingerprint *out_fingerprint) {
  return CryptoResult([&]() -> VxCoreError {
    const auto json = SerializeObject(header);
    if (!data_key.IsValid()) {
      return VXCORE_ERR_ENCRYPTION_LOCKED;
    }
    const auto aad = ObjectAad(header);
    SecureBytes record;
    auto error = record.Allocate(kRecordBytes);
    if (error != VXCORE_OK) {
      return error;
    }
    StreamState stream;
    if (!stream.state) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }
    std::array<unsigned char, crypto_secretstream_xchacha20poly1305_HEADERBYTES> stream_header{};
    if (crypto_secretstream_xchacha20poly1305_init_push(stream.state, stream_header.data(),
                                                      data_key.data_) != 0) {
      return VXCORE_ERR_NOT_INITIALIZED;
    }
    AtomicFileWriter writer(path);
    error = writer.Open();
    if (error == VXCORE_OK) {
      error = WriteStorageHeader(writer, kObjectMagic, json);
    }
    if (error == VXCORE_OK) {
      error = writer.Write(stream_header.data(), stream_header.size());
    }
    if (error != VXCORE_OK) {
      return error;
    }
    crypto_hash_sha256_state fingerprint{};
    if (out_fingerprint) {
      crypto_hash_sha256_init(&fingerprint);
      HashStorageHeader(fingerprint, reinterpret_cast<const unsigned char *>(json.data()), json.size());
      crypto_hash_sha256_update(&fingerprint, stream_header.data(), stream_header.size());
    }
    std::array<unsigned char, kRecordBytes + kStreamOverhead> ciphertext{};
    for (;;) {
      plaintext.read(reinterpret_cast<char *>(record.data_), kRecordBytes);
      if (plaintext.bad() || (plaintext.fail() && !plaintext.eof())) {
        return VXCORE_ERR_IO;
      }
      const auto count = plaintext.gcount();
      if (count < 0 || count > static_cast<std::streamsize>(kRecordBytes)) {
        return VXCORE_ERR_IO;
      }
      const bool final = plaintext.eof();
      unsigned long long ciphertext_size = 0;
      const auto tag = static_cast<unsigned char>(
          final ? crypto_secretstream_xchacha20poly1305_TAG_FINAL
                : crypto_secretstream_xchacha20poly1305_TAG_MESSAGE);
      if (crypto_secretstream_xchacha20poly1305_push(
              stream.state, ciphertext.data(), &ciphertext_size, record.data_,
              static_cast<unsigned long long>(count),
              reinterpret_cast<const unsigned char *>(aad.data()), aad.size(), tag) != 0) {
        return VXCORE_ERR_NOT_INITIALIZED;
      }
      sodium_memzero(record.data_, kRecordBytes);
      if (ciphertext_size != static_cast<unsigned long long>(count) + kStreamOverhead) {
        return VXCORE_ERR_NOT_INITIALIZED;
      }
      const auto size = Little32(static_cast<uint32_t>(ciphertext_size));
      error = writer.Write(size.data(), size.size());
      if (error == VXCORE_OK) {
        error = writer.Write(ciphertext.data(), static_cast<size_t>(ciphertext_size));
      }
      if (error != VXCORE_OK) {
        return error;
      }
      if (out_fingerprint) {
        crypto_hash_sha256_update(&fingerprint, size.data(), size.size());
        crypto_hash_sha256_update(&fingerprint, ciphertext.data(), ciphertext_size);
      }
      if (final) {
        error = writer.Commit();
        if (error == VXCORE_OK && out_fingerprint) {
          crypto_hash_sha256_final(&fingerprint, out_fingerprint->data());
        }
        return error;
      }
    }
  });
}

VxCoreError NotebookEncryption::DecryptRecords(std::istream &input, const ObjectHeader &header,
                                               const Key &data_key, const PlaintextSink &sink,
                                               const PlaintextSink &ciphertext_sink) {
  std::array<unsigned char, crypto_secretstream_xchacha20poly1305_HEADERBYTES> stream_header{};
  auto error = ReadExact(input, stream_header.data(), stream_header.size());
  if (error != VXCORE_OK) {
    return error;
  }
  if (ciphertext_sink) {
    error = ciphertext_sink(stream_header.data(), stream_header.size());
    if (error != VXCORE_OK) {
      return error;
    }
  }
  SecureBytes record;
  error = record.Allocate(kRecordBytes);
  if (error != VXCORE_OK) {
    return error;
  }
  StreamState stream;
  if (!stream.state) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  }
  if (crypto_secretstream_xchacha20poly1305_init_pull(stream.state, stream_header.data(),
                                                    data_key.data_) != 0) {
    return VXCORE_ERR_ENCRYPTION_AUTH_FAILED;
  }
  const auto aad = ObjectAad(header);
  std::array<unsigned char, kRecordBytes + kStreamOverhead> ciphertext{};
  for (;;) {
    std::array<unsigned char, 4> length{};
    error = ReadExact(input, length.data(), length.size());
    if (error != VXCORE_OK) {
      return error;  // EOF without an authenticated FINAL is never success.
    }
    const auto size = ReadLittle32(length.data());
    Require(size >= kStreamOverhead && size <= ciphertext.size());
    error = ReadExact(input, ciphertext.data(), size);
    if (error != VXCORE_OK) {
      return error;
    }
    if (ciphertext_sink) {
      error = ciphertext_sink(length.data(), length.size());
      if (error == VXCORE_OK) {
        error = ciphertext_sink(ciphertext.data(), size);
      }
      if (error != VXCORE_OK) {
        return error;
      }
    }
    unsigned char tag = 0;
    unsigned long long plaintext_size = 0;
    if (crypto_secretstream_xchacha20poly1305_pull(
            stream.state, record.data_, &plaintext_size, &tag, ciphertext.data(), size,
            reinterpret_cast<const unsigned char *>(aad.data()), aad.size()) != 0) {
      return VXCORE_ERR_ENCRYPTION_AUTH_FAILED;
    }
    Require(plaintext_size == size - kStreamOverhead);
    const bool final = tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL;
    Require(final || tag == crypto_secretstream_xchacha20poly1305_TAG_MESSAGE);
    Require(final || plaintext_size == kRecordBytes);
    if (final) {
      error = RequireEnd(input);
      if (error != VXCORE_OK) {
        return error;
      }
    }
    error = sink(record.data_, static_cast<size_t>(plaintext_size));
    sodium_memzero(record.data_, kRecordBytes);
    if (error != VXCORE_OK || final) {
      return error;
    }
  }
}

VxCoreError NotebookEncryption::ReadObject(const std::filesystem::path &path,
                                           const ObjectHeader &expected, const Key &data_key,
                                           size_t max_bytes, SecureBytes &out_plaintext,
                                           Fingerprint *out_fingerprint) {
  out_plaintext.Reset();
  return CryptoResult([&]() -> VxCoreError {
    ValidateObject(expected);
    if (!data_key.IsValid()) {
      return VXCORE_ERR_ENCRYPTION_LOCKED;
    }
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
      return VXCORE_ERR_IO;
    }
    const std::streamoff file_size = input.tellg();
    if (file_size < 0) {
      return VXCORE_ERR_IO;
    }
    input.seekg(0);
    if (!input) {
      return VXCORE_ERR_IO;
    }
    std::array<unsigned char, kMaxHeaderBytes> bytes{};
    uint32_t header_size = 0;
    auto error = ReadStorageHeader(input, kObjectMagic, bytes, header_size);
    if (error != VXCORE_OK) {
      return error;
    }
    auto header = ParseObject(bytes.data(), header_size);
    Require(SameObject(header, expected));
    // Ciphertext size is an upper bound on plaintext. Allocate once, capped by the
    // caller; no unbounded growth or abandoned realloc copies of plaintext exist.
    const auto capacity = static_cast<size_t>(std::min<uintmax_t>(
        static_cast<uintmax_t>(file_size), static_cast<uintmax_t>(max_bytes)));
    SecureBytes result;
    error = result.Allocate(capacity);
    if (error != VXCORE_OK) {
      return error;
    }
    crypto_hash_sha256_state fingerprint{};
    PlaintextSink ciphertext_sink;
    if (out_fingerprint) {
      crypto_hash_sha256_init(&fingerprint);
      HashStorageHeader(fingerprint, bytes.data(), header_size);
      ciphertext_sink = [&](const unsigned char *data, size_t size) {
        crypto_hash_sha256_update(&fingerprint, data, size);
        return VXCORE_OK;
      };
    }
    error = DecryptRecords(input, header, data_key, [&](const unsigned char *data, size_t size) {
      if (size > result.capacity_ - result.size_) {
        return VXCORE_ERR_ENCRYPTION_FORMAT;
      }
      if (size) {
        std::memcpy(result.data_ + result.size_, data, size);
      }
      result.size_ += size;
      return VXCORE_OK;
    }, ciphertext_sink);
    if (error == VXCORE_OK) {
      out_plaintext = std::move(result);
      if (out_fingerprint) {
        crypto_hash_sha256_final(&fingerprint, out_fingerprint->data());
      }
    }
    return error;
  });
}

VxCoreError NotebookEncryption::VerifyObject(const std::filesystem::path &path,
                                             const ObjectHeader &expected, const Key &data_key) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateObject(expected);
    if (!data_key.IsValid()) {
      return VXCORE_ERR_ENCRYPTION_LOCKED;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      return VXCORE_ERR_IO;
    }
    std::array<unsigned char, kMaxHeaderBytes> bytes{};
    uint32_t header_size = 0;
    auto error = ReadStorageHeader(input, kObjectMagic, bytes, header_size);
    if (error != VXCORE_OK) {
      return error;
    }
    const auto header = ParseObject(bytes.data(), header_size);
    Require(SameObject(header, expected));
    return DecryptRecords(input, header, data_key,
                          [](const unsigned char *, size_t) { return VXCORE_OK; });
  });
}

VxCoreError NotebookEncryption::FingerprintBytes(const void *bytes, size_t size,
                                                 Fingerprint &out_fingerprint) {
  const auto error = EnsureSodium();
  if (error != VXCORE_OK) {
    return error;
  }
  if (!bytes && size) {
    return VXCORE_ERR_NULL_POINTER;
  }
  crypto_hash_sha256(out_fingerprint.data(), static_cast<const unsigned char *>(bytes), size);
  return VXCORE_OK;
}


VxCoreError NotebookEncryption::RewrapSnapshot(
    const std::filesystem::path &source, const ObjectHeader &expected, const Key &note_key,
    const std::filesystem::path &destination, const ObjectHeader &replacement) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateObject(expected);
    const auto json = SerializeObject(replacement);
    if (!note_key.IsValid()) {
      return VXCORE_ERR_ENCRYPTION_LOCKED;
    }
    if (ObjectAad(expected) != ObjectAad(replacement)) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    std::ifstream input(source, std::ios::binary);
    if (!input) return VXCORE_ERR_IO;
    std::array<unsigned char, kMaxHeaderBytes> bytes{};
    uint32_t header_size = 0;
    auto error = ReadStorageHeader(input, kObjectMagic, bytes, header_size);
    if (error != VXCORE_OK) return error;
    Require(SameObject(ParseObject(bytes.data(), header_size), expected));
    AtomicFileWriter writer(destination);
    error = writer.Open();
    if (error == VXCORE_OK) error = WriteStorageHeader(writer, kObjectMagic, json);
    if (error != VXCORE_OK) return error;
    error = DecryptRecords(input, expected, note_key,
        [](const unsigned char *, size_t) { return VXCORE_OK; },
        [&](const unsigned char *data, size_t size) { return writer.Write(data, size); });
    input.close();
    return error == VXCORE_OK ? writer.Commit() : error;
  });
}

VxCoreError NotebookEncryption::TransferNote(
    const std::filesystem::path &source, const KeyEnvelope &source_envelope,
    const Key &source_notebook_key, const std::filesystem::path &destination,
    const KeyEnvelope &destination_envelope, const Key &destination_notebook_key, bool copy,
    const std::filesystem::path &backup_destination, bool &out_has_backup,
    const BodyTransform &transform_body) {
  out_has_backup = false;
  return CryptoResult([&]() -> VxCoreError {
    ValidateEnvelope(source_envelope);
    ValidateEnvelope(destination_envelope);
    ObjectHeader header;
    auto error = ReadObjectHeader(source, header);
    if (error != VXCORE_OK) return error;
    Require(header.kind == "note");
    Key source_key;
    error = UnwrapNoteKey(source_envelope.notebook_id, source_envelope.notebook_key_id,
                          source_notebook_key, header, source_key);
    if (error != VXCORE_OK) return error;
    SecretVector body;
    SecretJson manifest;
    error = ReadNoteSnapshot(source, header, source_key, body.value, manifest.value);
    if (error != VXCORE_OK) return error;
    const auto &editor = manifest.value.at(kJsonKeyEditorType).get_ref<const std::string &>();
    const bool body_changed = transform_body && transform_body(body.value, editor);
    Key copied_key;
    std::string document_id = header.document_id;
    if (copy) {
      error = GenerateKey(copied_key);
      if (error == VXCORE_OK) error = GenerateIdentity(document_id);
      if (error != VXCORE_OK) return error;
    }
    const auto &destination_key = copy ? copied_key : source_key;
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) return VXCORE_ERR_IO;
    const auto source_backup = PathFromUtf8(PathToUtf8(source) + ".vswp");
    const bool has_backup = std::filesystem::exists(source_backup, ec);
    if (ec) return VXCORE_ERR_IO;
    if (has_backup) {
      if (CheckReparsePoint(PathToUtf8(source_backup)) != ReparseState::kNo) {
        return VXCORE_ERR_INVALID_PARAM;
      }
      ObjectHeader backup_header;
      error = ReadObjectHeader(source_backup, backup_header);
      if (error != VXCORE_OK) return error;
      Require(backup_header.kind == "backup" && backup_header.document_id == header.document_id);
      Key backup_key;
      error = UnwrapNoteKey(source_envelope.notebook_id, source_envelope.notebook_key_id,
                            source_notebook_key, backup_header, backup_key);
      if (error != VXCORE_OK) return error;
      Require(KeysEqual(source_key, backup_key));
      SecretVector backup_body;
      SecretJson backup_manifest;
      int revision = 0;
      error = ReadNoteSnapshot(source_backup, backup_header, source_key, backup_body.value,
                               backup_manifest.value, nullptr, &revision);
      if (error != VXCORE_OK) return error;
      Require(backup_manifest.value.at(kJsonKeyEditorType) == editor);
      const bool backup_changed = transform_body && transform_body(backup_body.value, editor);
      std::filesystem::create_directories(backup_destination.parent_path(), ec);
      if (ec) return VXCORE_ERR_IO;
      if (copy || backup_changed) {
        error = WriteNoteSnapshot(backup_destination, destination_envelope,
            destination_notebook_key, destination_key, document_id, backup_body.value,
            backup_manifest.value, nullptr, nullptr, revision);
      } else {
        auto wrapped_backup = backup_header;
        error = WrapNoteKey(destination_envelope.notebook_id, destination_envelope.notebook_key_id,
                            destination_notebook_key, source_key, wrapped_backup);
        if (error == VXCORE_OK) error = RewrapSnapshot(source_backup, backup_header, source_key,
            backup_destination, wrapped_backup);
      }
      if (error != VXCORE_OK) return error;
      out_has_backup = true;
    }
    if (copy || body_changed) {
      return WriteNoteSnapshot(destination, destination_envelope, destination_notebook_key,
                               destination_key, document_id, body.value, manifest.value);
    }
    auto replacement = header;
    error = WrapNoteKey(destination_envelope.notebook_id, destination_envelope.notebook_key_id,
                        destination_notebook_key, source_key, replacement);
    return error == VXCORE_OK
        ? RewrapSnapshot(source, header, source_key, destination, replacement)
        : error;
  });
}

VxCoreError NotebookEncryption::GenerateIdentity(std::string &out_uuid) {
  return CryptoResult([&]() -> VxCoreError {
    const auto error = EnsureSodium();
    if (error == VXCORE_OK) {
      out_uuid = CryptoUuid();
    }
    return error;
  });
}

void NotebookEncryption::WipeBytes(std::vector<uint8_t> &bytes) noexcept {
  // Resize within capacity does not allocate; also erase storage left by a shrink.
  bytes.resize(bytes.capacity());
  if (!bytes.empty()) {
    sodium_memzero(bytes.data(), bytes.size());
  }
  bytes.clear();
}

void NotebookEncryption::WipeString(std::string &text) noexcept {
  // Includes moved-from small-string storage and capacity left after replacement.
  text.resize(text.capacity());
  if (!text.empty()) {
    sodium_memzero(&text[0], text.size());
  }
  text.clear();
}

void NotebookEncryption::WipeJson(nlohmann::json &json) noexcept {
  if (json.is_string()) {
    WipeString(json.get_ref<std::string &>());
  } else if (json.is_structured()) {
    for (auto &value : json) {
      WipeJson(value);
    }
  }
  json.clear();
}


VxCoreError NotebookEncryption::FingerprintObject(const std::filesystem::path &path,
                                                  Fingerprint &out_fingerprint) {
  return CryptoResult([&]() -> VxCoreError {
    auto error = EnsureSodium();
    if (error != VXCORE_OK) {
      return error;
    }
    std::ifstream input;
    input.rdbuf()->pubsetbuf(nullptr, 0);
    input.open(path, std::ios::binary);
    if (!input) {
      std::error_code ec;
      const bool exists = std::filesystem::exists(path, ec);
      return !exists && !ec ? VXCORE_ERR_NODE_NOT_EXISTS : VXCORE_ERR_IO;
    }
    crypto_hash_sha256_state state{};
    crypto_hash_sha256_init(&state);
    struct WipedRecord {
      std::array<unsigned char, kRecordBytes> value{};
      ~WipedRecord() { sodium_memzero(value.data(), value.size()); }
    } record;
    auto &bytes = record.value;
    for (;;) {
      input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
      if (input.bad() || (input.fail() && !input.eof())) {
        return VXCORE_ERR_IO;
      }
      const auto count = input.gcount();
      if (count > 0) {
        crypto_hash_sha256_update(&state, bytes.data(), static_cast<size_t>(count));
      }
      if (input.eof()) {
        crypto_hash_sha256_final(&state, out_fingerprint.data());
        return VXCORE_OK;
      }
    }
  });
}

VxCoreError NotebookEncryption::ValidateNoteManifest(const nlohmann::json &manifest) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateManifest(manifest);
    return VXCORE_OK;
  });
}

VxCoreError NotebookEncryption::WriteNoteSnapshot(
    const std::filesystem::path &path, const KeyEnvelope &envelope, const Key &notebook_key,
    const Key &note_key, const std::string &document_id, const std::vector<uint8_t> &body,
    const nlohmann::json &manifest, ObjectHeader *out_header,
    Fingerprint *out_fingerprint, int backup_revision) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateEnvelope(envelope);
    ValidateManifest(manifest);
    Require(IsCanonicalUuid(document_id) && backup_revision >= -1);
    SecretString serialized;
    serialized.value = Json{{kJsonKeyEditorType, manifest.at(kJsonKeyEditorType)}}.dump();
    Require(serialized.value.size() <= std::numeric_limits<uint32_t>::max());
    ObjectHeader header;
    header.kind = backup_revision >= 0 ? "backup" : "note";
    header.document_id = document_id;
    auto error = GenerateIdentity(header.object_id);
    if (error != VXCORE_OK) {
      return error;
    }
    error = WrapNoteKey(envelope.notebook_id, envelope.notebook_key_id, notebook_key, note_key, header);
    if (error != VXCORE_OK) {
      return error;
    }
    std::array<unsigned char, 8> body_length{};
    const auto size = static_cast<uint64_t>(body.size());
    for (size_t index = 0; index < body_length.size(); ++index) {
      body_length[index] = static_cast<unsigned char>(size >> (index * 8));
    }
    const auto manifest_length = Little32(static_cast<uint32_t>(serialized.value.size()));
    const auto revision = Little32(static_cast<uint32_t>(backup_revision));
    const SegmentedInput::Segment segments[] = {
        {revision.data(), backup_revision >= 0 ? revision.size() : 0},
        {body_length.data(), body_length.size()}, {body.data(), body.size()},
        {manifest_length.data(), manifest_length.size()},
        {serialized.value.data(), serialized.value.size()}};
    SegmentedInput buffer(segments, std::size(segments));
    std::istream input(&buffer);
    error = EncryptObject(path, header, note_key, input, out_fingerprint);
    if (error == VXCORE_OK && out_header) {
      *out_header = std::move(header);
    }
    return error;
  });
}

VxCoreError NotebookEncryption::ReadNoteSnapshot(
    const std::filesystem::path &path, const ObjectHeader &expected, const Key &note_key,
    std::vector<uint8_t> &out_body, nlohmann::json &out_manifest,
    Fingerprint *out_fingerprint, int *out_revision) {
  return CryptoResult([&]() -> VxCoreError {
    Require(CarriesNoteKey(expected));
    SecureBytes payload;
    Fingerprint fingerprint{};
    auto error = ReadObject(path, expected, note_key, std::numeric_limits<size_t>::max(),
                            payload, out_fingerprint ? &fingerprint : nullptr);
    if (error != VXCORE_OK) {
      return error;
    }
    const auto *bytes = payload.Data();
    size_t offset = 0;
    int revision = -1;
    if (expected.kind == "backup") {
      Require(payload.Size() >= 4);
      const auto value = ReadLittle32(bytes);
      Require(value <= static_cast<uint32_t>(std::numeric_limits<int>::max()));
      revision = static_cast<int>(value);
      offset = 4;
    }
    Require(payload.Size() - offset >= 12);
    uint64_t body_size = 0;
    for (size_t index = 0; index < 8; ++index) {
      body_size |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8);
    }
    offset += 8;
    Require(body_size <= payload.Size() - offset - 4);
    const auto *body = bytes + offset;
    offset += static_cast<size_t>(body_size);
    const auto manifest_size = ReadLittle32(bytes + offset);
    offset += 4;
    Require(manifest_size != 0 && manifest_size == payload.Size() - offset);
    SecretJson manifest;
    manifest.value = ParseManifest(bytes + offset, manifest_size);
    SecretVector candidate;
    candidate.value.assign(body, body + static_cast<size_t>(body_size));
    WipeBytes(out_body);
    out_body.swap(candidate.value);
    WipeJson(out_manifest);
    out_manifest.swap(manifest.value);
    if (out_fingerprint) {
      *out_fingerprint = fingerprint;
    }
    if (out_revision) {
      *out_revision = revision;
    }
    return VXCORE_OK;
  });
}

VxCoreError NotebookEncryption::InstallNotebookKey(const KeyEnvelope &envelope, Key &&notebook_key) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateEnvelope(envelope);
    if (!notebook_key.IsValid()) {
      return VXCORE_ERR_ENCRYPTION_LOCKED;
    }
    auto envelope_copy = envelope;
    auto key = std::make_shared<Key>(std::move(notebook_key));
    std::lock_guard<std::mutex> lock(mutex_);
    if (locking_) {
      return VXCORE_ERR_INVALID_STATE;
    }
    if (notebook_key_) {
      // Do not invalidate live leases or silently replace an unlocked hierarchy.
      if (envelope_.vault_id != envelope.vault_id || envelope_.notebook_id != envelope.notebook_id ||
          envelope_.notebook_key_id != envelope.notebook_key_id ||
          !KeysEqual(*notebook_key_, *key)) {
        return VXCORE_ERR_INVALID_STATE;
      }
    }
    envelope_ = std::move(envelope_copy);
    if (!notebook_key_) {
      notebook_key_ = std::move(key);
    }
    return VXCORE_OK;
  });
}

VxCoreError NotebookEncryption::AcquireNotebookKey(std::shared_ptr<const Key> &out_key,
                                                  KeyEnvelope *out_envelope) const {
  out_key.reset();
  return CryptoResult([&]() -> VxCoreError {
    std::lock_guard<std::mutex> lock(mutex_);
    if (locking_ || !notebook_key_) {
      return VXCORE_ERR_ENCRYPTION_LOCKED;
    }
    if (out_envelope) {
      *out_envelope = envelope_;
    }
    out_key = notebook_key_;
    return VXCORE_OK;
  });
}

VxCoreError NotebookEncryption::LockNotebookKey() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (locking_ || (notebook_key_ && notebook_key_.use_count() != 1)) {
    return VXCORE_ERR_INVALID_STATE;
  }
  locking_ = true;
  auto key = std::move(notebook_key_);
  lock.unlock();
  key.reset();  // Wipe outside the mutex; installs remain excluded until complete.
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
