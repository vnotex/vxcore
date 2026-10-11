#include "encryptednote.h"

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

#include "utils/file_utils.h"

namespace vxcore {

namespace {

constexpr unsigned char kKeyMagic[8] = {'V', 'N', 'E', 'K', 'E', 'Y', '1', 0};
constexpr unsigned char kObjectMagic[8] = {'V', 'N', 'O', 'T', 'E', 'E', '1', 0};
constexpr uint64_t kKdfOps = 3;
constexpr uint64_t kKdfMemory = 67108864;
constexpr size_t kStreamOverhead = crypto_secretstream_xchacha20poly1305_ABYTES;
using Encryption = EncryptedNote;
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
  Require(header.version == 1 || header.version == 2);
  if (header.version == 2) {
    ValidateEnvelope(header.envelope);
    Require(header.notebook_key_id == header.envelope.notebook_key_id);
  }
  Require(CarriesNoteKey(header));
  Require(Encryption::IsCanonicalUuid(header.document_id));
  Require(Encryption::IsCanonicalUuid(header.object_id));
  Require(Encryption::IsCanonicalUuid(header.notebook_key_id));
}

std::string ObjectAad(const Encryption::ObjectHeader &header) {
  return Json::array({"vnote-object", 1, header.kind, header.document_id, header.object_id}).dump();
}

std::string NoteKeyAad(const std::string &notebook_id, const Encryption::ObjectHeader &header) {
  if (header.version == 2) {
    const auto &envelope = header.envelope;
    return Json::array({"vnote-note-key", 2, envelope.notebook_id, header.notebook_key_id,
                        header.document_id, header.kind, header.object_id, envelope.vault_id,
                        "argon2id13", kKdfOps, kKdfMemory, Base64(envelope.salt),
                        Base64(envelope.master_key.nonce), Base64(envelope.master_key.ciphertext),
                        Base64(envelope.notebook_key.nonce),
                        Base64(envelope.notebook_key.ciphertext)}).dump();
  }
  return Json::array({"vnote-note-key", 1, notebook_id, header.notebook_key_id, header.document_id,
                      header.kind, header.object_id}).dump();
}

void AddEnvelope(Json &json, const Encryption::KeyEnvelope &envelope) {
  ValidateEnvelope(envelope);
  json[kJsonKeyVaultId] = envelope.vault_id;
  json[kJsonKeyNotebookId] = envelope.notebook_id;
  json[kJsonKeyNotebookKeyId] = envelope.notebook_key_id;
  json[kJsonKeyKdf] = "argon2id13";
  json[kJsonKeyOpslimit] = kKdfOps;
  json[kJsonKeyMemlimit] = kKdfMemory;
  json[kJsonKeySalt] = Base64(envelope.salt);
  json[kJsonKeyMasterNonce] = Base64(envelope.master_key.nonce);
  json[kJsonKeyWrappedMasterKey] = Base64(envelope.master_key.ciphertext);
  json[kJsonKeyNotebookNonce] = Base64(envelope.notebook_key.nonce);
  json[kJsonKeyWrappedNotebookKey] = Base64(envelope.notebook_key.ciphertext);
}

Encryption::KeyEnvelope ParseEnvelope(const Json &json) {
  Require(json.at(kJsonKeyKdf).is_string() && json.at(kJsonKeyKdf) == "argon2id13");
  Require(IsInteger(json.at(kJsonKeyOpslimit), kKdfOps) &&
          IsInteger(json.at(kJsonKeyMemlimit), kKdfMemory));
  Encryption::KeyEnvelope envelope;
  envelope.vault_id = UuidField(json, kJsonKeyVaultId);
  envelope.notebook_id = UuidField(json, kJsonKeyNotebookId);
  envelope.notebook_key_id = UuidField(json, kJsonKeyNotebookKeyId);
  DecodeBase64(json.at(kJsonKeySalt), envelope.salt);
  DecodeBase64(json.at(kJsonKeyMasterNonce), envelope.master_key.nonce);
  DecodeBase64(json.at(kJsonKeyWrappedMasterKey), envelope.master_key.ciphertext);
  DecodeBase64(json.at(kJsonKeyNotebookNonce), envelope.notebook_key.nonce);
  DecodeBase64(json.at(kJsonKeyWrappedNotebookKey), envelope.notebook_key.ciphertext);
  return envelope;
}

std::string SerializeObject(const Encryption::ObjectHeader &header) {
  ValidateObject(header);
  Json json = {{kJsonKeyVersion, header.version}, {kJsonKeyKind, header.kind}, {kJsonKeyDocumentId, header.document_id},
               {kJsonKeyObjectId, header.object_id}};
  json[kJsonKeyNotebookKeyId] = header.notebook_key_id;
  json[kJsonKeyKeyNonce] = Base64(header.note_key.nonce);
  json[kJsonKeyWrappedNoteKey] = Base64(header.note_key.ciphertext);
  if (header.version == 2) AddEnvelope(json, header.envelope);
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
  if (IsInteger(json.at(kJsonKeyVersion), 1)) {
    ExactKeys(json, {kJsonKeyVersion, kJsonKeyKind, kJsonKeyDocumentId, kJsonKeyObjectId,
                     kJsonKeyNotebookKeyId, kJsonKeyKeyNonce, kJsonKeyWrappedNoteKey});
  } else {
    Require(IsInteger(json.at(kJsonKeyVersion), 2));
    ExactKeys(json, {kJsonKeyVersion, kJsonKeyKind, kJsonKeyDocumentId, kJsonKeyObjectId,
                     kJsonKeyNotebookKeyId, kJsonKeyKeyNonce, kJsonKeyWrappedNoteKey,
                     kJsonKeyVaultId, kJsonKeyNotebookId, kJsonKeyKdf, kJsonKeyOpslimit,
                     kJsonKeyMemlimit, kJsonKeySalt, kJsonKeyMasterNonce,
                     kJsonKeyWrappedMasterKey, kJsonKeyNotebookNonce, kJsonKeyWrappedNotebookKey});
    header.version = 2;
    header.envelope = ParseEnvelope(json);
  }
  header.notebook_key_id = UuidField(json, kJsonKeyNotebookKeyId);
  DecodeBase64(json[kJsonKeyKeyNonce], header.note_key.nonce);
  DecodeBase64(json[kJsonKeyWrappedNoteKey], header.note_key.ciphertext);
  header.document_id = UuidField(json, kJsonKeyDocumentId);
  header.object_id = UuidField(json, kJsonKeyObjectId);
  ValidateObject(header);
  return header;
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

// Check the canonical-parent snapshot selected by migration. Reject links at
// every component, including links installed after resolving a trusted alias.
// Missing is accepted only for the final component (an absent backup is normal).
VxCoreError CheckSnapshotPath(const std::filesystem::path &path, bool &exists) {
  exists = false;
  if (path.empty()) return VXCORE_ERR_INVALID_PARAM;
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  if (ec) return VXCORE_ERR_IO;
  for (const auto &part : absolute) {
    if (part == "..") return VXCORE_ERR_INVALID_PARAM;
  }
  const auto clean = absolute.lexically_normal();
  auto current = clean.root_path();
  const auto root_reparse = CheckReparsePoint(PathToUtf8(current));
  if (root_reparse != ReparseState::kNo) {
    return root_reparse == ReparseState::kError ? VXCORE_ERR_IO : VXCORE_ERR_INVALID_PARAM;
  }
  for (const auto &part : clean.relative_path()) {
    current /= part;
    const auto status = std::filesystem::symlink_status(current, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == std::filesystem::file_type::not_found)) {
      return current == clean ? VXCORE_OK : VXCORE_ERR_IO;
    }
    if (ec) return VXCORE_ERR_IO;
    const auto reparse = CheckReparsePoint(PathToUtf8(current));
    if (reparse != ReparseState::kNo) {
      return reparse == ReparseState::kError ? VXCORE_ERR_IO : VXCORE_ERR_INVALID_PARAM;
    }
    if (current == clean) {
      if (!std::filesystem::is_regular_file(status)) return VXCORE_ERR_ENCRYPTION_FORMAT;
    } else if (!std::filesystem::is_directory(status)) {
      return VXCORE_ERR_INVALID_PARAM;
    }
  }
  exists = true;
  return VXCORE_OK;
}

}  // namespace

bool EncryptedNote::SameEnvelope(const KeyEnvelope &left, const KeyEnvelope &right) noexcept {
  return left.vault_id == right.vault_id && left.notebook_id == right.notebook_id &&
         left.notebook_key_id == right.notebook_key_id && left.salt == right.salt &&
         left.master_key.nonce == right.master_key.nonce &&
         left.master_key.ciphertext == right.master_key.ciphertext &&
         left.notebook_key.nonce == right.notebook_key.nonce &&
         left.notebook_key.ciphertext == right.notebook_key.ciphertext;
}

bool EncryptedNote::SameObject(const ObjectHeader &left, const ObjectHeader &right) noexcept {
  return left.version == right.version && left.kind == right.kind &&
         left.document_id == right.document_id && left.object_id == right.object_id &&
         left.notebook_key_id == right.notebook_key_id &&
         left.note_key.nonce == right.note_key.nonce &&
         left.note_key.ciphertext == right.note_key.ciphertext &&
         (left.version != 2 || SameEnvelope(left.envelope, right.envelope));
}

VxCoreError EncryptedNote::MakeSelfContained(ObjectHeader &header, const KeyEnvelope &envelope,
                                            const Key &notebook_key, const Key &note_key) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateEnvelope(envelope);
    auto replacement = header;
    replacement.version = 2;
    replacement.envelope = envelope;
    auto error = WrapNoteKey(envelope.notebook_id, envelope.notebook_key_id,
                             notebook_key, note_key, replacement);
    if (error == VXCORE_OK) header = std::move(replacement);
    return error;
  });
}

EncryptedNote::Key::~Key() { Reset(); }

EncryptedNote::Key::Key(Key &&other) noexcept : data_(std::exchange(other.data_, nullptr)) {}

EncryptedNote::Key &EncryptedNote::Key::operator=(Key &&other) noexcept {
  if (this != &other) {
    Reset();
    data_ = std::exchange(other.data_, nullptr);
  }
  return *this;
}

void EncryptedNote::Key::Reset() noexcept {
  if (data_) {
    sodium_free(data_);  // Wipes the entire guarded allocation before unlocking/freeing it.
    data_ = nullptr;
  }
}

EncryptedNote::SecureBytes::~SecureBytes() { Reset(); }

EncryptedNote::SecureBytes::SecureBytes(SecureBytes &&other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
      capacity_(std::exchange(other.capacity_, 0)) {}

EncryptedNote::SecureBytes &EncryptedNote::SecureBytes::operator=(SecureBytes &&other) noexcept {
  if (this != &other) {
    Reset();
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
    capacity_ = std::exchange(other.capacity_, 0);
  }
  return *this;
}

void EncryptedNote::SecureBytes::Reset() noexcept {
  if (data_) {
    sodium_free(data_);  // sodium_free wipes capacity, including unauthenticated/unused bytes.
    data_ = nullptr;
  }
  size_ = capacity_ = 0;
}

VxCoreError EncryptedNote::SecureBytes::Allocate(size_t capacity) {
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

bool EncryptedNote::IsCanonicalUuid(const std::string &value) noexcept {
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

VxCoreError EncryptedNote::AllocateKey(Key &out_key) {
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

VxCoreError EncryptedNote::GenerateKey(Key &out_key) {
  Key key;
  auto error = AllocateKey(key);
  if (error == VXCORE_OK) {
    randombytes_buf(key.data_, kKeyBytes);
    out_key = std::move(key);
  }
  return error;
}

bool EncryptedNote::KeysEqual(const Key &left, const Key &right) noexcept {
  return left.IsValid() && right.IsValid() &&
         sodium_memcmp(left.data_, right.data_, kKeyBytes) == 0;
}

VxCoreError EncryptedNote::DeriveKey(const void *password, size_t password_size,
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

VxCoreError EncryptedNote::WrapKey(const Key &wrapping_key, const Key &key,
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

VxCoreError EncryptedNote::UnwrapKey(const Key &wrapping_key, const WrappedKey &wrapped,
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

VxCoreError EncryptedNote::PrepareNewKeys(const std::string &notebook_id, const void *password,
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

VxCoreError EncryptedNote::PrepareNotebookKeys(const KeyEnvelope &authenticated_source,
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

VxCoreError EncryptedNote::UnlockKeys(const KeyEnvelope &envelope, const std::string &notebook_id,
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

VxCoreError EncryptedNote::UnlockNotebookKey(const KeyEnvelope &envelope,
                                                   const Key &master_key, Key &out_notebook_key) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateEnvelope(envelope);
    if (&master_key == &out_notebook_key) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    return UnwrapKey(master_key, envelope.notebook_key, NotebookAad(envelope), out_notebook_key);
  });
}

VxCoreError EncryptedNote::EncodeKeyEnvelope(const KeyEnvelope &envelope,
                                                  std::string &out_bytes) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateEnvelope(envelope);
    Json json = {{kJsonKeyVersion, 1}};
    AddEnvelope(json, envelope);
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

VxCoreError EncryptedNote::DecodeKeyEnvelope(const void *bytes, size_t size,
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
    out_envelope = ParseEnvelope(json);
    return VXCORE_OK;
  });
}

VxCoreError EncryptedNote::ReadKeyEnvelope(const std::filesystem::path &path,
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

VxCoreError EncryptedNote::WrapNoteKey(const std::string &notebook_id,
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
    Require(header.version != 2 || header.envelope.notebook_id == notebook_id);
    auto error = WrapKey(notebook_key, note_key, NoteKeyAad(notebook_id, header), header.note_key);
    if (error == VXCORE_OK) {
      in_out_header = std::move(header);
    }
    return error;
  });
}

VxCoreError EncryptedNote::UnwrapNoteKey(const std::string &notebook_id,
                                             const std::string &notebook_key_id,
                                             const Key &notebook_key, const ObjectHeader &header,
                                             Key &out_note_key) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateObject(header);
    if (!CarriesNoteKey(header) || !IsCanonicalUuid(notebook_id) ||
        notebook_key_id != header.notebook_key_id ||
        (header.version == 2 && notebook_id != header.envelope.notebook_id)) {
      return VXCORE_ERR_ENCRYPTION_FORMAT;
    }
    if (&notebook_key == &out_note_key) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    return UnwrapKey(notebook_key, header.note_key, NoteKeyAad(notebook_id, header), out_note_key);
  });
}

VxCoreError EncryptedNote::ReadObjectHeader(const std::filesystem::path &path,
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

VxCoreError EncryptedNote::EncryptObject(const std::filesystem::path &path,
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

VxCoreError EncryptedNote::DecryptRecords(std::istream &input, const ObjectHeader &header,
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

VxCoreError EncryptedNote::ReadObject(const std::filesystem::path &path,
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

VxCoreError EncryptedNote::VerifyObject(const std::filesystem::path &path,
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

VxCoreError EncryptedNote::FingerprintBytes(const void *bytes, size_t size,
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


VxCoreError EncryptedNote::RewrapSnapshot(
    const std::filesystem::path &source, const ObjectHeader &expected, const Key &note_key,
    const std::filesystem::path &destination, const ObjectHeader &replacement,
    const Fingerprint *expected_fingerprint,
    const std::function<VxCoreError()> &before_commit, Fingerprint *out_fingerprint) {
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
    crypto_hash_sha256_state original_hash{}, replacement_hash{};
    if (expected_fingerprint) {
      crypto_hash_sha256_init(&original_hash);
      HashStorageHeader(original_hash, bytes.data(), header_size);
    }
    if (out_fingerprint) {
      crypto_hash_sha256_init(&replacement_hash);
      HashStorageHeader(replacement_hash, reinterpret_cast<const unsigned char *>(json.data()),
                        json.size());
    }
    error = DecryptRecords(input, expected, note_key,
        [](const unsigned char *, size_t) { return VXCORE_OK; },
        [&](const unsigned char *data, size_t size) {
          if (expected_fingerprint) crypto_hash_sha256_update(&original_hash, data, size);
          if (out_fingerprint) crypto_hash_sha256_update(&replacement_hash, data, size);
          return writer.Write(data, size);
        });
    input.close();
    if (error != VXCORE_OK) return error;
    if (expected_fingerprint) {
      Fingerprint actual{};
      crypto_hash_sha256_final(&original_hash, actual.data());
      if (actual != *expected_fingerprint) return VXCORE_ERR_FILE_CHANGED_OUTSIDE;
    }
    if (before_commit) {
      error = before_commit();
      if (error != VXCORE_OK) return error;
    }
    error = writer.Commit();
    if (error == VXCORE_OK && out_fingerprint) {
      crypto_hash_sha256_final(&replacement_hash, out_fingerprint->data());
    }
    return error;
  });
}

VxCoreError EncryptedNote::MigrateNote(const std::filesystem::path &path,
                                      const KeyEnvelope &envelope, const Key &notebook_key,
                                      bool &changed) {
  changed = false;
  return CryptoResult([&]() -> VxCoreError {
    if (path.empty()) return VXCORE_ERR_INVALID_PARAM;
    for (const auto &part : path) {
      if (part == "..") return VXCORE_ERR_INVALID_PARAM;
    }
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec).lexically_normal();
    if (ec) return VXCORE_ERR_IO;
    // The caller validates notebook containment. Resolve only the trusted parent
    // once so all reads, rechecks and publications use the same physical target,
    // even if an ancestor alias is retargeted. Never resolve the final file.
    const auto parent = std::filesystem::canonical(absolute.parent_path(), ec);
    if (ec) return VXCORE_ERR_IO;
    const auto snapshot_path = parent / absolute.filename();
    bool exists = false;
    auto error = CheckSnapshotPath(snapshot_path, exists);
    if (error != VXCORE_OK) return error;
    if (!exists) return VXCORE_ERR_NODE_NOT_EXISTS;
    ObjectHeader header;
    error = ReadObjectHeader(snapshot_path, header);
    if (error != VXCORE_OK) return error;
    Require(header.kind == "note");
    if (header.version == 2) return VXCORE_OK;
    ValidateEnvelope(envelope);
    Key note_key;
    error = UnwrapNoteKey(envelope.notebook_id, envelope.notebook_key_id,
                          notebook_key, header, note_key);
    if (error != VXCORE_OK) return error;
    SecretJson manifest;
    Fingerprint note_fingerprint{};
    error = ReadNoteSnapshotImpl(snapshot_path, header, note_key, nullptr, manifest.value,
                                 &note_fingerprint, nullptr);
    if (error != VXCORE_OK) return error;
    const auto backup_path = PathFromUtf8(PathToUtf8(snapshot_path) + ".vswp");
    bool has_backup = false;
    error = CheckSnapshotPath(backup_path, has_backup);
    if (error != VXCORE_OK) return error;
    ObjectHeader backup_header;
    Fingerprint backup_fingerprint{};
    if (has_backup) {
      error = ReadObjectHeader(backup_path, backup_header);
      if (error != VXCORE_OK) return error;
      Require(backup_header.kind == "backup" && backup_header.document_id == header.document_id);
      Require(backup_header.version != 2 || SameEnvelope(backup_header.envelope, envelope));
      Key backup_key;
      error = UnwrapNoteKey(envelope.notebook_id, envelope.notebook_key_id,
                            notebook_key, backup_header, backup_key);
      if (error != VXCORE_OK) return error;
      Require(KeysEqual(note_key, backup_key));
      SecretJson backup_manifest;
      int revision = -1;
      error = ReadNoteSnapshotImpl(backup_path, backup_header, note_key, nullptr,
                                   backup_manifest.value, &backup_fingerprint, &revision);
      if (error != VXCORE_OK) return error;
      Require(backup_manifest.value.at(kJsonKeyEditorType) == manifest.value.at(kJsonKeyEditorType));
    }
    // Recheck both original files, including backup absence, immediately before
    // each durable publication. A retry accepts an already-upgraded backup.
    auto revalidate = [&]() -> VxCoreError {
      for (int index = 0; index != 2; ++index) {
        const auto &candidate = index == 0 ? snapshot_path : backup_path;
        bool present = false;
        auto result = CheckSnapshotPath(candidate, present);
        if (result != VXCORE_OK) return result;
        if (present != (index == 0 || has_backup)) return VXCORE_ERR_FILE_CHANGED_OUTSIDE;
        if (!present) continue;
        Fingerprint actual{};
        result = FingerprintObject(candidate, actual);
        if (result != VXCORE_OK) return result;
        if (actual != (index == 0 ? note_fingerprint : backup_fingerprint)) {
          return VXCORE_ERR_FILE_CHANGED_OUTSIDE;
        }
      }
      return VXCORE_OK;
    };
    auto replacement = header;
    error = MakeSelfContained(replacement, envelope, notebook_key, note_key);
    if (error != VXCORE_OK) return error;
    if (has_backup && backup_header.version == 1) {
      auto replacement_backup = backup_header;
      error = MakeSelfContained(replacement_backup, envelope, notebook_key, note_key);
      if (error != VXCORE_OK) return error;
      Fingerprint upgraded_fingerprint{};
      error = RewrapSnapshot(backup_path, backup_header, note_key, backup_path,
                              replacement_backup, &backup_fingerprint, revalidate,
                              &upgraded_fingerprint);
      if (error != VXCORE_OK) return error;
      changed = true;
      backup_fingerprint = upgraded_fingerprint;
    }
    error = RewrapSnapshot(snapshot_path, header, note_key, snapshot_path, replacement,
                            &note_fingerprint, revalidate);
    if (error == VXCORE_OK) changed = true;
    return error;
  });
}

VxCoreError EncryptedNote::TransferNote(
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
    Require(header.version != 2 || SameEnvelope(header.envelope, source_envelope));
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
      Require(backup_header.version != 2 || SameEnvelope(backup_header.envelope, source_envelope));
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
        error = MakeSelfContained(wrapped_backup, destination_envelope,
                                  destination_notebook_key, source_key);
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
    error = MakeSelfContained(replacement, destination_envelope,
                              destination_notebook_key, source_key);
    return error == VXCORE_OK
        ? RewrapSnapshot(source, header, source_key, destination, replacement)
        : error;
  });
}

VxCoreError EncryptedNote::GenerateIdentity(std::string &out_uuid) {
  return CryptoResult([&]() -> VxCoreError {
    const auto error = EnsureSodium();
    if (error == VXCORE_OK) {
      out_uuid = CryptoUuid();
    }
    return error;
  });
}

void EncryptedNote::WipeBytes(std::vector<uint8_t> &bytes) noexcept {
  // Resize within capacity does not allocate; also erase storage left by a shrink.
  bytes.resize(bytes.capacity());
  if (!bytes.empty()) {
    sodium_memzero(bytes.data(), bytes.size());
  }
  bytes.clear();
}

void EncryptedNote::WipeString(std::string &text) noexcept {
  // Includes moved-from small-string storage and capacity left after replacement.
  text.resize(text.capacity());
  if (!text.empty()) {
    sodium_memzero(&text[0], text.size());
  }
  text.clear();
}

void EncryptedNote::WipeJson(nlohmann::json &json) noexcept {
  if (json.is_string()) {
    WipeString(json.get_ref<std::string &>());
  } else if (json.is_structured()) {
    for (auto &value : json) {
      WipeJson(value);
    }
  }
  json.clear();
}


VxCoreError EncryptedNote::FingerprintObject(const std::filesystem::path &path,
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

VxCoreError EncryptedNote::ValidateNoteManifest(const nlohmann::json &manifest) {
  return CryptoResult([&]() -> VxCoreError {
    ValidateManifest(manifest);
    return VXCORE_OK;
  });
}

VxCoreError EncryptedNote::WriteNoteSnapshot(
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
    error = MakeSelfContained(header, envelope, notebook_key, note_key);
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

VxCoreError EncryptedNote::ReadNoteSnapshot(
    const std::filesystem::path &path, const ObjectHeader &expected, const Key &note_key,
    std::vector<uint8_t> &out_body, nlohmann::json &out_manifest,
    Fingerprint *out_fingerprint, int *out_revision) {
  return ReadNoteSnapshotImpl(path, expected, note_key, &out_body, out_manifest,
                              out_fingerprint, out_revision);
}

VxCoreError EncryptedNote::ReadNoteSnapshotImpl(
    const std::filesystem::path &path, const ObjectHeader &expected, const Key &note_key,
    std::vector<uint8_t> *out_body, nlohmann::json &out_manifest,
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
    if (out_body) {
      SecretVector candidate;
      candidate.value.assign(body, body + static_cast<size_t>(body_size));
      WipeBytes(*out_body);
      out_body->swap(candidate.value);
    }
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

}  // namespace vxcore
