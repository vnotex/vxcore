#include "notebook_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <unordered_map>

#include <vxcore/notebook_json_keys.h>

#include "bundled_notebook.h"
#include "config_manager.h"
#include "core/event_manager.h"
#include "core/event_names.h"
#include "core/folder_manager.h"
#include "metadata_store.h"
#include "raw_notebook.h"
#include "sync/git/git_conflict_resolver.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

namespace {

using Encryption = NotebookEncryption;
namespace fs = std::filesystem;

bool SameEnvelope(const Encryption::KeyEnvelope &a, const Encryption::KeyEnvelope &b) {
  return a.vault_id == b.vault_id && a.notebook_id == b.notebook_id &&
         a.notebook_key_id == b.notebook_key_id && a.salt == b.salt &&
         a.master_key.nonce == b.master_key.nonce &&
         a.master_key.ciphertext == b.master_key.ciphertext &&
         a.notebook_key.nonce == b.notebook_key.nonce &&
         a.notebook_key.ciphertext == b.notebook_key.ciphertext;
}

fs::path EncryptionKeyPath(const Notebook &notebook) {
  return PathFromUtf8(notebook.GetMetadataFolder()) / "encryption.vne";
}

VxCoreError EntryExists(const fs::path &path, bool &exists) {
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  if (ec == std::errc::no_such_file_or_directory) {
    exists = false;
    return VXCORE_OK;
  }
  if (ec) {
    return VXCORE_ERR_IO;
  }
  exists = status.type() != fs::file_type::not_found;
  return VXCORE_OK;
}

VxCoreError CheckNotebookIdentity(const Notebook &notebook, bool writable,
                                  fs::path &out_root) {
  if (writable) {
    const auto error = notebook.CheckWritable();
    if (error != VXCORE_OK) return error;
  }
  if (notebook.GetType() != NotebookType::Bundled) {
    return VXCORE_ERR_UNSUPPORTED;
  }
  const auto conflict_error = GitConflictResolver::CheckEncryptionKeyConflict(
      notebook.GetMetadataFolder() + "/vx_sync");
  if (conflict_error != VXCORE_OK) return conflict_error;
  if (!Encryption::IsCanonicalUuid(notebook.GetId())) {
    return VXCORE_ERR_ENCRYPTION_FORMAT;
  }
  const auto root = PathFromUtf8(notebook.GetRootFolder());
  const auto metadata = PathFromUtf8(notebook.GetMetadataFolder());
  for (const auto &path : {root, metadata, metadata / "config.json"}) {
    const auto state = CheckReparsePoint(PathToUtf8(path));
    if (state != ReparseState::kNo) {
      return state == ReparseState::kError ? VXCORE_ERR_IO : VXCORE_ERR_INVALID_STATE;
    }
  }
  std::error_code ec;
  out_root = fs::canonical(root, ec);
  if (ec) {
    return VXCORE_ERR_IO;
  }
  nlohmann::json config;
  auto error = LoadJsonFile(metadata / "config.json", config);
  if (error != VXCORE_OK) {
    return error;
  }
  if (!config.is_object() || !config.contains(kJsonKeyId) ||
      config[kJsonKeyId] != notebook.GetId()) {
    return VXCORE_ERR_INVALID_STATE;
  }
  return VXCORE_OK;
}

VxCoreError ReadNotebookEnvelope(const Notebook &notebook, Encryption::KeyEnvelope &envelope,
                                 std::string *out_bytes = nullptr) {
  const auto path = EncryptionKeyPath(notebook);
  bool exists = false;
  auto error = EntryExists(path, exists);
  if (error != VXCORE_OK || !exists) {
    return error != VXCORE_OK ? error : VXCORE_ERR_ENCRYPTION_RECOVERY_REQUIRED;
  }
  if (CheckReparsePoint(PathToUtf8(path)) != ReparseState::kNo) {
    return VXCORE_ERR_ENCRYPTION_FORMAT;
  }
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) {
    return ec ? VXCORE_ERR_IO : VXCORE_ERR_ENCRYPTION_FORMAT;
  }
  std::string bytes;
  error = ReadFileHead(path, Encryption::kMaxHeaderBytes + 1, bytes);
  if (error == VXCORE_OK) {
    error = Encryption::DecodeKeyEnvelope(bytes.data(), bytes.size(), envelope);
  }
  if (error == VXCORE_OK && envelope.notebook_id != notebook.GetId()) {
    return VXCORE_ERR_ENCRYPTION_FORMAT;
  }
  if (error == VXCORE_OK && out_bytes) {
    *out_bytes = std::move(bytes);
  }
  return error;
}

// Only explicit initialization/status of a missing key file takes this slow path.
// Never initialize over orphaned ciphertext or an encrypted FileRecord marker.
VxCoreError CheckNoProtectedContent(const Notebook &notebook) {
  std::error_code ec;
  fs::recursive_directory_iterator it(PathFromUtf8(notebook.GetRootFolder()), ec), end;
  if (ec) {
    return VXCORE_ERR_IO;
  }
  while (it != end) {
    const auto path = it->path();
    const auto name = path.filename();
    if (name == ".git") {
      it.disable_recursion_pending();
    } else {
      const auto state = CheckReparsePoint(PathToUtf8(path));
      if (state == ReparseState::kError) {
        return VXCORE_ERR_IO;
      }
      if (state == ReparseState::kYes) {
        return VXCORE_ERR_INVALID_STATE;
      }
      if (path.extension() == ".vne") {
        return VXCORE_ERR_ENCRYPTION_RECOVERY_REQUIRED;
      }
      const auto relative = path.lexically_relative(PathFromUtf8(notebook.GetMetadataFolder()));
      if (name == "vx.json" && !relative.empty() && *relative.begin() == "contents") {
        nlohmann::json config;
        auto error = LoadJsonFile(path, config);
        if (error != VXCORE_OK) {
          return error;
        }
        if (!config.is_object() || (config.contains(kJsonKeyFiles) &&
                                   !config[kJsonKeyFiles].is_array())) {
          return VXCORE_ERR_ENCRYPTION_FORMAT;
        }
        if (config.contains(kJsonKeyFiles)) {
          for (const auto &file : config[kJsonKeyFiles]) {
            if (!file.is_object()) {
              return VXCORE_ERR_ENCRYPTION_FORMAT;
            }
            const auto metadata = file.find(kJsonKeyMetadata);
            if (metadata != file.end() && metadata->is_object() &&
                metadata->contains(kJsonKeyEncrypted)) {
              const auto &marker = metadata->at(kJsonKeyEncrypted);
              if (!marker.is_boolean()) {
                return VXCORE_ERR_ENCRYPTION_FORMAT;
              }
              if (marker.get<bool>()) {
                return VXCORE_ERR_ENCRYPTION_RECOVERY_REQUIRED;
              }
            }
          }
        }
      }
    }
    it.increment(ec);
    if (ec) {
      return VXCORE_ERR_IO;
    }
  }
  return VXCORE_OK;
}

VxCoreError CheckKeyFileAbsent(const Notebook &notebook) {
  bool exists = false;
  auto error = EntryExists(EncryptionKeyPath(notebook), exists);
  return error != VXCORE_OK ? error : (exists ? VXCORE_ERR_ALREADY_EXISTS : VXCORE_OK);
}

VxCoreError EnsureEncryptedGitAttributes(const Notebook &notebook) {
  const auto path = PathFromUtf8(notebook.GetRootFolder()) / ".gitattributes";
  bool exists = false;
  auto error = EntryExists(path, exists);
  if (error != VXCORE_OK) {
    return error;
  }
  std::string bytes;
  if (exists) {
    if (CheckReparsePoint(PathToUtf8(path)) != ReparseState::kNo) {
      return VXCORE_ERR_INVALID_STATE;
    }
    // Generic ReadFile intentionally retains its ordinary text-mode semantics.
    // Encryption policy edits must preserve existing CRLF and every unrelated byte.
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
      return VXCORE_ERR_IO;
    }
    const auto size = input.tellg();
    if (size < 0) {
      return VXCORE_ERR_IO;
    }
    bytes.resize(static_cast<size_t>(size));
    input.seekg(0);
    if (!bytes.empty()) {
      input.read(&bytes[0], static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) {
      return VXCORE_ERR_IO;
    }
    if (input.peek() != std::char_traits<char>::eof() || input.bad()) {
      return VXCORE_ERR_IO;
    }
  }
  // Only the last effective line is accepted: a preceding rule could be overridden
  // by later user rules. Appending preserves every existing byte and makes this
  // initialization's binary policy take precedence in this attributes file.
  const std::string rule = "*.vne -text -diff -merge";
  std::string last_line;
  std::istringstream lines(bytes);
  for (std::string line; std::getline(lines, line);) {
    const auto first = line.find_first_not_of(" \t\r");
    if (first != std::string::npos && line[first] != '#') {
      const auto last = line.find_last_not_of(" \t\r");
      last_line = line.substr(first, last - first + 1);
    }
  }
  if (last_line == rule) {
    return VXCORE_OK;
  }
  if (!bytes.empty() && bytes.back() != '\n') {
    bytes.push_back('\n');
  }
  bytes += rule + "\n";
  return WriteFileAtomic(path, bytes);
}

VxCoreError PublishKeyFile(const Notebook &notebook, const Encryption::KeyEnvelope &envelope,
                           const std::string &bytes) {
  // An exclusively created ciphertext-only staging directory avoids replacing any
  // pre-existing path. Final publication is atomic NO-REPLACE, not check+rename.
  const auto destination = EncryptionKeyPath(notebook);
  const auto directory = destination.parent_path() /
                         PathFromUtf8(".encryption-" + envelope.notebook_key_id);
  const auto staged = directory / "encryption.vne";
  std::error_code ec;
  if (!fs::create_directory(directory, ec)) {
    return ec ? VXCORE_ERR_IO : VXCORE_ERR_ALREADY_EXISTS;
  }
  struct StagingCleanup {
    fs::path file;
    fs::path directory;
    ~StagingCleanup() {
      std::error_code ignored;
      fs::remove(file, ignored);
      fs::remove(directory, ignored);
    }
  } cleanup{staged, directory};
  auto error = WriteFileAtomic(staged, bytes);
  if (error != VXCORE_OK) {
    return error;
  }
#ifdef _WIN32
  if (!MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
    const auto win_error = GetLastError();
    return win_error == ERROR_ALREADY_EXISTS || win_error == ERROR_FILE_EXISTS
               ? VXCORE_ERR_ALREADY_EXISTS : VXCORE_ERR_IO;
  }
#else
  // link() is an atomic no-replace publication of this already-flushed regular
  // file on the same filesystem. Cleanup unlinks the private staging name.
  fs::create_hard_link(staged, destination, ec);
  if (ec) {
    return ec == std::errc::file_exists ? VXCORE_ERR_ALREADY_EXISTS : VXCORE_ERR_IO;
  }
#endif
  return VXCORE_OK;
}

}  // namespace

struct NotebookManager::EncryptionSetup {
  struct Prepared {
    std::string notebook_id;
    std::string source_notebook_id;
    fs::path root;
    fs::path source_root;
    std::shared_ptr<Encryption> owner;
    std::shared_ptr<Encryption> source_owner;
    Encryption::KeyEnvelope envelope;
    std::string key_file_bytes;
    std::string source_key_file_bytes;
    std::shared_ptr<const Encryption::Key> master_key;
    std::shared_ptr<const Encryption::Key> notebook_key;
  };
  // Empty after commit/free. Retain just this shell until context teardown so an
  // old handle can never alias a newly allocated setup during this context lifetime.
  std::unique_ptr<Prepared> prepared;
};

struct NotebookManager::EncryptionSession {
  struct MasterKeyEntry {
    // vaultId is the map key. These are public authenticated password-envelope
    // fields only; never retain a password or derived KEK in the session.
    std::array<unsigned char, 16> salt;
    Encryption::WrappedKey master_key;
    std::shared_ptr<const Encryption::Key> key;

    MasterKeyEntry(const Encryption::KeyEnvelope &envelope,
                   std::shared_ptr<const Encryption::Key> value)
        : salt(envelope.salt), master_key(envelope.master_key), key(std::move(value)) {}

    bool Matches(const Encryption::KeyEnvelope &envelope) const noexcept {
      return salt == envelope.salt && master_key.nonce == envelope.master_key.nonce &&
             master_key.ciphertext == envelope.master_key.ciphertext;
    }
  };
  std::mutex mutex;
  std::map<std::string, MasterKeyEntry> master_keys;
  std::vector<std::shared_ptr<Encryption>> owners;
  std::map<EncryptionSetup *, std::unique_ptr<EncryptionSetup>> setups;
  size_t active_operations = 0;
  bool publishing = false;
  bool locking = false;

  struct Operation {
    EncryptionSession &session;
    bool active = false;
    explicit Operation(EncryptionSession &value) : session(value) {
      std::lock_guard<std::mutex> lock(session.mutex);
      if (!session.locking) {
        ++session.active_operations;
        active = true;
      }
    }
    ~Operation() {
      if (active) {
        std::lock_guard<std::mutex> lock(session.mutex);
        --session.active_operations;
      }
    }
  };
};

std::shared_ptr<NotebookManager::EncryptionSession>
NotebookManager::GetEncryptionSession(bool create) const {
  auto session = std::atomic_load(&encryption_session_);
  if (!session && create) {
    auto candidate = std::make_shared<EncryptionSession>();
    if (std::atomic_compare_exchange_strong(&encryption_session_, &session, candidate)) {
      session = std::move(candidate);
    }
  }
  return session;
}

VxCoreError NotebookManager::RegisterEncryptionOwner(Notebook &notebook,
                                                     EncryptionSession &session) {
  NotebookEncryption *owner = nullptr;
  auto error = notebook.EnsureEncryption(owner);
  if (error != VXCORE_OK) {
    return error;
  }
  std::lock_guard<std::mutex> lock(session.mutex);
  for (const auto &entry : session.owners) {
    if (entry.get() == owner) {
      return VXCORE_OK;
    }
  }
  session.owners.push_back(std::atomic_load(&notebook.encryption_));
  return VXCORE_OK;
}

VxCoreError NotebookManager::InstallMasterKey(
    const Encryption::KeyEnvelope &authenticated_envelope, Encryption::Key &&master_key) {
  const auto &vault_id = authenticated_envelope.vault_id;
  if (!Encryption::IsCanonicalUuid(vault_id)) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  if (!master_key.IsValid()) {
    return VXCORE_ERR_ENCRYPTION_LOCKED;
  }
  try {
    auto session = GetEncryptionSession(true);
    auto key = std::make_shared<Encryption::Key>(std::move(master_key));
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->locking || session->publishing) {
      return VXCORE_ERR_INVALID_STATE;
    }
    const auto found = session->master_keys.find(vault_id);
    if (found != session->master_keys.end()) {
      return found->second.Matches(authenticated_envelope) &&
                     Encryption::KeysEqual(*found->second.key, *key)
                 ? VXCORE_OK : VXCORE_ERR_ENCRYPTION_AUTH_FAILED;
    }
    session->master_keys.emplace(vault_id,
                                 EncryptionSession::MasterKeyEntry(authenticated_envelope,
                                                                    std::move(key)));
    return VXCORE_OK;
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  }
}

VxCoreError NotebookManager::AcquireMasterKey(
    const std::string &vault_id, std::shared_ptr<const Encryption::Key> &out_key) const {
  out_key.reset();
  if (!Encryption::IsCanonicalUuid(vault_id)) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  auto session = GetEncryptionSession(false);
  if (!session) {
    return VXCORE_ERR_ENCRYPTION_LOCKED;
  }
  std::lock_guard<std::mutex> lock(session->mutex);
  const auto found = session->master_keys.find(vault_id);
  if (session->locking || found == session->master_keys.end()) {
    return VXCORE_ERR_ENCRYPTION_LOCKED;
  }
  out_key = found->second.key;
  return VXCORE_OK;
}

VxCoreError NotebookManager::LockMasterKeys() { return LockAllEncryption(); }

VxCoreError NotebookManager::LockAllEncryption() {
  auto session = GetEncryptionSession(false);
  if (!session) {
    return VXCORE_OK;
  }
  decltype(EncryptionSession::master_keys) master_keys;
  std::vector<std::shared_ptr<const Encryption::Key>> notebook_keys;
  std::unique_lock<std::mutex> lock(session->mutex);
  if (session->active_operations || session->publishing || session->locking) {
    return VXCORE_ERR_INVALID_STATE;
  }
  for (const auto &entry : session->setups) {
    if (entry.second->prepared) {
      return VXCORE_ERR_INVALID_STATE;
    }
  }
  std::vector<std::unique_lock<std::mutex>> owner_locks;
  owner_locks.reserve(session->owners.size());
  notebook_keys.reserve(session->owners.size());
  for (const auto &owner : session->owners) {
    owner_locks.emplace_back(owner->mutex_);
  }
  // Hold ALL sparse owner locks before checking ANY key, preventing a new lease
  // between an earlier preflight and a later clear. No ordinary notebook walk.
  for (const auto &entry : session->master_keys) {
    if (entry.second.key.use_count() != 1) {
      return VXCORE_ERR_INVALID_STATE;
    }
  }
  for (const auto &owner : session->owners) {
    if (owner->locking_ || (owner->notebook_key_ && owner->notebook_key_.use_count() != 1)) {
      return VXCORE_ERR_INVALID_STATE;
    }
  }
  session->locking = true;
  for (const auto &owner : session->owners) {
    owner->locking_ = true;
    notebook_keys.push_back(std::move(owner->notebook_key_));
  }
  master_keys.swap(session->master_keys);
  owner_locks.clear();
  lock.unlock();
  notebook_keys.clear();
  master_keys.clear();  // Wipe guarded allocations without any registry mutex held.
  lock.lock();
  for (const auto &owner : session->owners) {
    std::lock_guard<std::mutex> owner_lock(owner->mutex_);
    owner->locking_ = false;
  }
  session->locking = false;
  return VXCORE_OK;
}

VxCoreError NotebookManager::PrepareNotebookEncryption(
    const std::string &notebook_id, const std::string &source_notebook_id,
    const void *password, size_t password_size, EncryptionSetup *&out_setup) {
  out_setup = nullptr;
  if ((!password && password_size) || notebook_id.empty()) {
    return !password && password_size ? VXCORE_ERR_NULL_POINTER : VXCORE_ERR_INVALID_PARAM;
  }
  auto session = GetEncryptionSession(true);
  EncryptionSession::Operation operation(*session);
  if (!operation.active) {
    return VXCORE_ERR_INVALID_STATE;
  }
  auto *notebook = GetNotebook(notebook_id);
  if (!notebook) {
    return VXCORE_ERR_NOT_FOUND;
  }
  auto prepared = std::make_unique<EncryptionSetup::Prepared>();
  auto error = CheckNotebookIdentity(*notebook, true, prepared->root);
  if (error == VXCORE_OK) {
    error = CheckKeyFileAbsent(*notebook);
  }
  if (error == VXCORE_OK) {
    error = CheckNoProtectedContent(*notebook);
  }
  if (error == VXCORE_OK) {
    error = RegisterEncryptionOwner(*notebook, *session);
  }
  if (error != VXCORE_OK) {
    return error;
  }
  prepared->notebook_id = notebook_id;
  prepared->owner = std::atomic_load(&notebook->encryption_);
  {
    std::lock_guard<std::mutex> lock(prepared->owner->mutex_);
    if (!prepared->owner->envelope_.notebook_id.empty()) {
      return VXCORE_ERR_ENCRYPTION_RECOVERY_REQUIRED;
    }
  }
  Encryption::Key master_key, notebook_key;
  if (source_notebook_id.empty()) {
    error = Encryption::PrepareNewKeys(notebook_id, password, password_size,
                                       prepared->envelope, master_key, notebook_key);
    if (error == VXCORE_OK) {
      prepared->master_key = std::make_shared<Encryption::Key>(std::move(master_key));
    }
  } else {
    if (source_notebook_id == notebook_id) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    auto *source = GetNotebook(source_notebook_id);
    if (!source) {
      return VXCORE_ERR_NOT_FOUND;
    }
    error = CheckNotebookIdentity(*source, false, prepared->source_root);
    Encryption::KeyEnvelope source_envelope;
    if (error == VXCORE_OK) {
      error = ReadNotebookEnvelope(*source, source_envelope, &prepared->source_key_file_bytes);
    }
    if (error == VXCORE_OK) {
      error = RegisterEncryptionOwner(*source, *session);
    }
    if (error != VXCORE_OK) {
      return error;
    }
    prepared->source_notebook_id = source_notebook_id;
    prepared->source_owner = std::atomic_load(&source->encryption_);
    if (password_size) {
      Encryption::Key source_key;
      error = Encryption::UnlockKeys(source_envelope, source_notebook_id, password,
                                     password_size, master_key, source_key);
      source_key.Reset();
      if (error == VXCORE_OK) {
        prepared->master_key = std::make_shared<Encryption::Key>(std::move(master_key));
      }
    } else {
      Encryption::KeyEnvelope authenticated_envelope;
      std::shared_ptr<const Encryption::Key> source_key;
      error = prepared->source_owner->AcquireNotebookKey(source_key, &authenticated_envelope);
      if (error == VXCORE_OK && !SameEnvelope(source_envelope, authenticated_envelope)) {
        return VXCORE_ERR_ENCRYPTION_AUTH_FAILED;
      }
      if (error == VXCORE_OK) {
        error = AcquireMasterKey(source_envelope.vault_id, prepared->master_key);
      }
    }
    if (error == VXCORE_OK) {
      error = Encryption::PrepareNotebookKeys(source_envelope, *prepared->master_key,
                                              notebook_id, prepared->envelope, notebook_key);
    }
  }
  if (error != VXCORE_OK) {
    return error;
  }
  prepared->notebook_key = std::make_shared<Encryption::Key>(std::move(notebook_key));
  error = Encryption::EncodeKeyEnvelope(prepared->envelope, prepared->key_file_bytes);
  if (error != VXCORE_OK) {
    return error;
  }
  auto setup = std::make_unique<EncryptionSetup>();
  setup->prepared = std::move(prepared);
  auto *handle = setup.get();
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->setups.emplace(handle, std::move(setup));
  }
  out_setup = handle;
  return VXCORE_OK;
}

VxCoreError NotebookManager::InstallEncryptionKeys(
    Notebook &notebook, EncryptionSession &session, const Encryption::KeyEnvelope &envelope,
    std::shared_ptr<const Encryption::Key> master_key,
    std::shared_ptr<const Encryption::Key> notebook_key, const std::string *key_file_bytes) {
  // All potentially allocating publication state is prepared BEFORE filesystem
  // changes. The registry reservation excludes other installs without holding a
  // mutex during filesystem IO. KDF workers and existing leases remain independent.
  auto owner = std::atomic_load(&notebook.encryption_);
  auto envelope_copy = envelope;
  decltype(EncryptionSession::master_keys) pending_master;
  pending_master.emplace(envelope.vault_id,
                         EncryptionSession::MasterKeyEntry(envelope, std::move(master_key)));
  {
    std::lock_guard<std::mutex> lock(session.mutex);
    std::lock_guard<std::mutex> owner_lock(owner->mutex_);
    if (session.locking || session.publishing || owner->locking_) {
      return VXCORE_ERR_INVALID_STATE;
    }
    const auto found = session.master_keys.find(envelope.vault_id);
    if (found != session.master_keys.end() &&
        (found->second.Matches(envelope) == false ||
         Encryption::KeysEqual(*found->second.key, *pending_master.begin()->second.key) == false)) {
      return VXCORE_ERR_ENCRYPTION_AUTH_FAILED;
    }
    if (owner->notebook_key_ &&
        (owner->envelope_.vault_id != envelope.vault_id ||
         owner->envelope_.notebook_id != envelope.notebook_id ||
         owner->envelope_.notebook_key_id != envelope.notebook_key_id ||
         !Encryption::KeysEqual(*owner->notebook_key_, *notebook_key))) {
      return VXCORE_ERR_INVALID_STATE;
    }
    session.publishing = true;
  }
  struct Publication {
    EncryptionSession &session;
    ~Publication() {
      std::lock_guard<std::mutex> lock(session.mutex);
      session.publishing = false;
    }
  } publication{session};
  if (key_file_bytes) {
    auto error = EnsureEncryptedGitAttributes(notebook);
    if (error == VXCORE_OK) {
      error = CheckKeyFileAbsent(notebook);
    }
    if (error == VXCORE_OK) {
      error = PublishKeyFile(notebook, envelope, *key_file_bytes);
    }
    if (error != VXCORE_OK) {
      return error;
    }
  }
  {
    std::lock_guard<std::mutex> lock(session.mutex);
    std::lock_guard<std::mutex> owner_lock(owner->mutex_);
    // Node insertion cannot allocate, and the reservation made the preflight
    // invariant stable while the mutex was released for publication.
    if (session.master_keys.find(envelope.vault_id) == session.master_keys.end()) {
      session.master_keys.insert(pending_master.extract(pending_master.begin()));
    }
    owner->envelope_ = std::move(envelope_copy);
    if (!owner->notebook_key_) {
      owner->notebook_key_ = std::move(notebook_key);
    }
  }
  return VXCORE_OK;
}

VxCoreError NotebookManager::CommitNotebookEncryption(EncryptionSetup *setup) {
  auto session = GetEncryptionSession(false);
  if (!session || !setup) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  EncryptionSession::Operation operation(*session);
  if (!operation.active) {
    return VXCORE_ERR_INVALID_STATE;
  }
  std::unique_ptr<EncryptionSetup::Prepared> prepared;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    const auto found = session->setups.find(setup);
    if (found == session->setups.end() || !found->second->prepared) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    prepared = std::move(found->second->prepared);
  }
  // From here EVERY exit consumes the handle and destroys its guarded keys.
  auto *notebook = GetNotebook(prepared->notebook_id);
  if (!notebook || notebook->GetEncryption() != prepared->owner.get()) {
    return VXCORE_ERR_INVALID_STATE;
  }
  fs::path root;
  auto error = CheckNotebookIdentity(*notebook, true, root);
  if (error == VXCORE_OK && root != prepared->root) {
    return VXCORE_ERR_INVALID_STATE;
  }
  if (error == VXCORE_OK) {
    error = CheckKeyFileAbsent(*notebook);
  }
  if (error == VXCORE_OK) {
    error = CheckNoProtectedContent(*notebook);
  }
  if (error != VXCORE_OK) {
    return error;
  }
  if (!prepared->source_notebook_id.empty()) {
    auto *source = GetNotebook(prepared->source_notebook_id);
    if (!source || source->GetEncryption() != prepared->source_owner.get()) {
      return VXCORE_ERR_INVALID_STATE;
    }
    error = CheckNotebookIdentity(*source, false, root);
    if (error != VXCORE_OK) {
      return error;
    }
    if (root != prepared->source_root) {
      return VXCORE_ERR_INVALID_STATE;
    }
    Encryption::KeyEnvelope source_envelope;
    std::string bytes;
    error = ReadNotebookEnvelope(*source, source_envelope, &bytes);
    if (error != VXCORE_OK) {
      return error;
    }
    if (bytes != prepared->source_key_file_bytes) {
      return VXCORE_ERR_INVALID_STATE;
    }
  }
  return InstallEncryptionKeys(*notebook, *session, prepared->envelope,
                                prepared->master_key, prepared->notebook_key,
                                &prepared->key_file_bytes);
}

void NotebookManager::FreeEncryptionSetup(EncryptionSetup *setup) {
  auto session = GetEncryptionSession(false);
  if (!session || !setup) {
    return;
  }
  // Count the wipe as an active operation, so Lock All cannot report success
  // after detachment but before the final guarded allocation has been erased.
  EncryptionSession::Operation operation(*session);
  std::unique_ptr<EncryptionSetup::Prepared> prepared;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    const auto found = session->setups.find(setup);
    if (found != session->setups.end()) {
      prepared = std::move(found->second->prepared);
    }
  }
}

VxCoreError NotebookManager::UnlockNotebookEncryption(const std::string &notebook_id,
                                                       const void *password,
                                                       size_t password_size) {
  if (!password && password_size) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto session = GetEncryptionSession(true);
  EncryptionSession::Operation operation(*session);
  if (!operation.active) {
    return VXCORE_ERR_INVALID_STATE;
  }
  auto *notebook = GetNotebook(notebook_id);
  if (!notebook) {
    return VXCORE_ERR_NOT_FOUND;
  }
  fs::path root;
  auto error = CheckNotebookIdentity(*notebook, false, root);
  Encryption::KeyEnvelope envelope;
  std::string bytes;
  if (error == VXCORE_OK) {
    error = ReadNotebookEnvelope(*notebook, envelope, &bytes);
  }
  Encryption::Key master_key, notebook_key;
  if (error == VXCORE_OK) {
    error = Encryption::UnlockKeys(envelope, notebook_id, password, password_size,
                                   master_key, notebook_key);
  }
  if (error != VXCORE_OK) {
    return error;
  }
  // A checkout/key-file replacement during KDF must not install a stale snapshot.
  fs::path current_root;
  error = CheckNotebookIdentity(*notebook, false, current_root);
  if (error != VXCORE_OK) {
    return error;
  }
  Encryption::KeyEnvelope current_envelope;
  std::string current_bytes;
  error = ReadNotebookEnvelope(*notebook, current_envelope, &current_bytes);
  if (error != VXCORE_OK) {
    return error;
  }
  if (current_root != root || current_bytes != bytes) {
    return VXCORE_ERR_INVALID_STATE;
  }
  error = RegisterEncryptionOwner(*notebook, *session);
  if (error != VXCORE_OK) {
    return error;
  }
  return InstallEncryptionKeys(*notebook, *session, envelope,
                                std::make_shared<Encryption::Key>(std::move(master_key)),
                                std::make_shared<Encryption::Key>(std::move(notebook_key)), nullptr);
}

VxCoreError NotebookManager::UnlockNotebookWithCachedMaster(const std::string &notebook_id) {
  // This entry point belongs exclusively to the protected-candidate open/load
  // path. Public unlock(password) still authenticates those exact password bytes.
  try {
    auto *notebook = GetNotebook(notebook_id);
    if (!notebook) {
      return VXCORE_ERR_NOT_FOUND;
    }
    if (notebook->GetType() != NotebookType::Bundled) {
      return VXCORE_ERR_UNSUPPORTED;
    }
    auto session = GetEncryptionSession(false);
    if (!session) {
      return VXCORE_ERR_ENCRYPTION_LOCKED;
    }
    EncryptionSession::Operation operation(*session);
    if (!operation.active) {
      return VXCORE_ERR_INVALID_STATE;
    }
    fs::path root;
    auto error = CheckNotebookIdentity(*notebook, false, root);
    Encryption::KeyEnvelope envelope;
    std::string bytes;
    if (error == VXCORE_OK) {
      error = ReadNotebookEnvelope(*notebook, envelope, &bytes);
    }
    if (error != VXCORE_OK) {
      return error;
    }
    std::shared_ptr<const Encryption::Key> master_key;
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      const auto found = session->master_keys.find(envelope.vault_id);
      if (found == session->master_keys.end()) {
        return VXCORE_ERR_ENCRYPTION_LOCKED;
      }
      if (found->second.Matches(envelope) == false) {
        return VXCORE_ERR_ENCRYPTION_AUTH_FAILED;
      }
      master_key = found->second.key;
    }
    Encryption::Key notebook_key;
    error = Encryption::UnlockNotebookKey(envelope, *master_key, notebook_key);
    if (error != VXCORE_OK) {
      return error;
    }
    // Revalidate both identity and complete key-file bytes before any installation;
    // the borrowed MK lease and active operation exclude Lock All throughout.
    fs::path current_root;
    error = CheckNotebookIdentity(*notebook, false, current_root);
    if (error != VXCORE_OK) {
      return error;
    }
    Encryption::KeyEnvelope current_envelope;
    std::string current_bytes;
    error = ReadNotebookEnvelope(*notebook, current_envelope, &current_bytes);
    if (error != VXCORE_OK) {
      return error;
    }
    if (current_root != root || current_bytes != bytes) {
      return VXCORE_ERR_INVALID_STATE;
    }
    error = RegisterEncryptionOwner(*notebook, *session);
    if (error != VXCORE_OK) {
      return error;
    }
    return InstallEncryptionKeys(*notebook, *session, envelope, std::move(master_key),
                                  std::make_shared<Encryption::Key>(std::move(notebook_key)), nullptr);
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::length_error &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_ENCRYPTION_FORMAT;
  } catch (const std::filesystem::filesystem_error &) {
    return VXCORE_ERR_IO;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::GetEncryptionStatus(const std::string &notebook_id,
                                                  const char *file_path,
                                                  std::string &out_status_json) {
  out_status_json.clear();
  auto *notebook = GetNotebook(notebook_id);
  if (!notebook) {
    return VXCORE_ERR_NOT_FOUND;
  }
  if (notebook->GetType() != NotebookType::Bundled) {
    return VXCORE_ERR_UNSUPPORTED;
  }
  const auto conflict_error = GitConflictResolver::CheckEncryptionKeyConflict(
      notebook->GetMetadataFolder() + "/vx_sync");
  if (conflict_error != VXCORE_OK) return conflict_error;
  bool encrypted = false;
  Encryption::ObjectHeader header;
  if (file_path) {
    if (!*file_path) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    const auto relative = notebook->GetCleanRelativePath(file_path);
    const auto relative_path = PathFromUtf8(relative);
    if (relative.empty() || relative_path.is_absolute()) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    for (const auto &part : relative_path) {
      if (part == "..") {
        return VXCORE_ERR_INVALID_PARAM;
      }
    }
    const bool suffix = relative_path.extension() == ".vne";
    const FileRecord *record = nullptr;
    auto error = notebook->GetFolderManager()->GetFileInfo(relative, &record);
    if (error != VXCORE_OK) {
      return suffix && error == VXCORE_ERR_NOT_FOUND ? VXCORE_ERR_ENCRYPTION_FORMAT : error;
    }
    if (record->metadata.is_object() && record->metadata.contains(kJsonKeyEncrypted)) {
      const auto &marker = record->metadata.at(kJsonKeyEncrypted);
      if (!marker.is_boolean()) {
        return VXCORE_ERR_ENCRYPTION_FORMAT;
      }
      encrypted = marker.get<bool>();
    }
    if (suffix != encrypted) {
      return VXCORE_ERR_ENCRYPTION_FORMAT;
    }
    if (encrypted) {
      const auto editor = record->metadata.find(kJsonKeyEditorType);
      if (editor == record->metadata.end() ||
          (*editor != "markdown" && *editor != "text" && *editor != "mindmap")) {
        return VXCORE_ERR_ENCRYPTION_FORMAT;
      }
      const auto absolute = PathFromUtf8(notebook->GetAbsolutePath(relative));
      if (!IsPathWithin(notebook->GetRootFolder(), PathToUtf8(absolute), false)) {
        return VXCORE_ERR_ENCRYPTION_FORMAT;
      }
      auto current = PathFromUtf8(notebook->GetRootFolder());
      for (const auto &part : relative_path) {
        current /= part;
        if (CheckReparsePoint(PathToUtf8(current)) != ReparseState::kNo) {
          return VXCORE_ERR_ENCRYPTION_FORMAT;
        }
      }
      error = Encryption::ReadObjectHeader(absolute, header);
      if (error != VXCORE_OK) {
        return error;
      }
      if (header.kind != "note") {
        return VXCORE_ERR_ENCRYPTION_FORMAT;
      }
    }
  }
  bool initialized = false;
  auto error = EntryExists(EncryptionKeyPath(*notebook), initialized);
  if (error != VXCORE_OK) {
    return error;
  }
  Encryption::KeyEnvelope envelope;
  bool unlocked = false;
  if (initialized) {
    error = ReadNotebookEnvelope(*notebook, envelope);
    if (error != VXCORE_OK) {
      return error;
    }
    if (encrypted && header.notebook_key_id != envelope.notebook_key_id) {
      return VXCORE_ERR_ENCRYPTION_FORMAT;
    }
    if (auto *owner = notebook->GetEncryption()) {
      std::lock_guard<std::mutex> lock(owner->mutex_);
      unlocked = !owner->locking_ && owner->notebook_key_ && SameEnvelope(envelope, owner->envelope_);
    }
  } else {
    if (encrypted) {
      return VXCORE_ERR_ENCRYPTION_RECOVERY_REQUIRED;
    }
    if (auto *owner = notebook->GetEncryption()) {
      std::lock_guard<std::mutex> lock(owner->mutex_);
      if (!owner->envelope_.notebook_id.empty()) {
        return VXCORE_ERR_ENCRYPTION_RECOVERY_REQUIRED;
      }
    }
    error = CheckNoProtectedContent(*notebook);
    if (error != VXCORE_OK) {
      return error;
    }
  }
  out_status_json = nlohmann::json{{kJsonKeyInitialized, initialized},
                                  {kJsonKeyUnlocked, unlocked},
                                  {kJsonKeyEncrypted, encrypted},
                                  {kJsonKeyVaultId, envelope.vault_id}}.dump();
  return VXCORE_OK;
}

NotebookManager::NotebookManager(ConfigManager *config_manager) : config_manager_(config_manager) {
  LoadOpenNotebooks();
}

NotebookManager::~NotebookManager() = default;

void NotebookManager::LoadOpenNotebooks() {
  auto &session_config = config_manager_->GetSessionConfig();
  const auto local_data_folder = config_manager_->GetLocalDataPath();
  notebooks_.clear();

  // Deterministic reconcile/dedupe: build a fresh record list where exactly one
  // record survives per cleaned root, a loadable record always wins over a
  // phantom, and a stale persisted id is healed against the loaded config.json
  // id. Phantoms (failed-to-load, e.g. folder not yet hydrated by OneDrive) are
  // preserved so a notebook is never silently lost.
  std::vector<NotebookRecord> reconciled;
  reconciled.reserve(session_config.notebooks.size());
  std::unordered_map<std::string, size_t> root_index;   // cleaned root -> index in reconciled
  std::unordered_map<std::string, bool> root_loaded;     // cleaned root -> slot is loaded vs phantom
  bool changed = false;

  for (const auto &record : session_config.notebooks) {
    const std::string root_clean = CleanPath(record.root_folder);

    std::unique_ptr<Notebook> notebook;
    switch (record.type) {
      case NotebookType::Bundled: {
        auto error = BundledNotebook::Open(local_data_folder, root_clean, notebook);
        if (error != VXCORE_OK) {
          VXCORE_LOG_ERROR("Failed to load bundled notebook: root_folder=%s, error=%d",
                           record.root_folder.c_str(), error);
        }
        break;
      }
      case NotebookType::Raw: {
        auto error = RawNotebook::Open(local_data_folder, root_clean, record.id, notebook);
        if (error != VXCORE_OK) {
          VXCORE_LOG_ERROR("Failed to load raw notebook: root_folder=%s, error=%d",
                           record.root_folder.c_str(), error);
        }
        break;
      }
      default:
        VXCORE_LOG_ERROR("Skip invalid notebook type: %d", static_cast<int>(record.type));
        break;
    }

    if (notebook) {
      // T15: re-apply the persisted per-device read-only flag before the
      // notebook is published to the manager's map, so a downstream
      // GetNotebook(id)->IsReadOnly() sees the same state the session was
      // shut down in. Missing field is back-compat (defaults to false).
      notebook->SetReadOnly(record.read_only);

      // Reconcile a stale persisted id against the ground-truth id loaded from
      // config.json (the root_folder is the stable identity on this device).
      NotebookRecord kept = record;
      if (record.id != notebook->GetId()) {
        VXCORE_LOG_WARN("LoadOpenNotebooks: reconciling stale record id %s -> %s for root=%s",
                        record.id.c_str(), notebook->GetId().c_str(), root_clean.c_str());
        kept.id = notebook->GetId();
        changed = true;
      }

      VXCORE_LOG_INFO("Loaded open notebook: id=%s, root_folder=%s, read_only=%d",
                      notebook->GetId().c_str(), notebook->GetRootFolder().c_str(),
                      record.read_only ? 1 : 0);

      if (root_clean.empty()) {
        // Malformed empty root: always keep, never dedupe/overwrite.
        reconciled.push_back(std::move(kept));
      } else {
        auto found = root_index.find(root_clean);
        if (found == root_index.end()) {
          // Root unseen: append and remember its slot as loaded.
          root_index[root_clean] = reconciled.size();
          root_loaded[root_clean] = true;
          reconciled.push_back(std::move(kept));
        } else if (!root_loaded[root_clean]) {
          // Existing slot is a phantom: a loadable record replaces it.
          reconciled[found->second] = std::move(kept);
          root_loaded[root_clean] = true;
          changed = true;
        } else {
          // Slot already loaded: drop this duplicate.
          changed = true;
        }
      }

      notebooks_[notebook->GetId()] = std::move(notebook);
    } else {
      // Phantom: KEEP the record (a load failure is often transient) but dedupe
      // duplicate phantoms / phantoms shadowed by a same-root loaded record.
      if (root_clean.empty()) {
        // Malformed empty root: always keep, never dedupe.
        VXCORE_LOG_WARN("LoadOpenNotebooks: keeping unloaded (phantom) record id=%s root=%s",
                        record.id.c_str(), root_clean.c_str());
        reconciled.push_back(record);
      } else if (root_index.find(root_clean) == root_index.end()) {
        // Root unseen: keep the phantom and remember its slot.
        VXCORE_LOG_WARN("LoadOpenNotebooks: keeping unloaded (phantom) record id=%s root=%s",
                        record.id.c_str(), root_clean.c_str());
        root_index[root_clean] = reconciled.size();
        root_loaded[root_clean] = false;
        reconciled.push_back(record);
      } else {
        // A loaded or earlier phantom for the same root already represents it.
        changed = true;
      }
    }
  }

  if (changed) {
    session_config.notebooks = std::move(reconciled);
    config_manager_->SaveSessionConfig();
  }
}

VxCoreError NotebookManager::CreateNotebook(const std::string &root_folder, NotebookType type,
                                            const std::string &config_json,
                                            std::string &out_notebook_id) {
  VXCORE_LOG_INFO("Creating notebook: root_folder=%s, type=%d", root_folder.c_str(),
                  static_cast<int>(type));

  const auto root_folder_clean = CleanPath(root_folder);

  try {
    auto rootPath = PathFromUtf8(root_folder_clean);
    if (!std::filesystem::exists(rootPath)) {
      std::filesystem::create_directories(rootPath);
      VXCORE_LOG_DEBUG("Created root directory: %s", root_folder_clean.c_str());
    } else if (type == NotebookType::Bundled &&
               // Notebook::kConfigFileName is protected/unreachable here; use the
               // literal "config.json" (its value, defined in notebook.cpp).
               IsRegularFile(ConcatenatePaths(
                   ConcatenatePaths(root_folder_clean, BundledNotebook::kMetadataFolderName),
                   "config.json"))) {
      VXCORE_LOG_WARN(
          "CreateNotebook: '%s' already holds a bundled notebook; treating as re-add, "
          "preserving existing id",
          root_folder_clean.c_str());
      return OpenNotebook(root_folder, out_notebook_id);
    }

    const auto local_data_folder = config_manager_->GetLocalDataPath();

    // At least there should be name.
    assert(!config_json.empty());
    nlohmann::json json = nlohmann::json::parse(config_json);
    auto config = NotebookConfig::FromJson(json);
    config.id.clear();

    std::unique_ptr<Notebook> notebook;
    switch (type) {
      case NotebookType::Bundled: {
        auto err = BundledNotebook::Create(local_data_folder, root_folder_clean, &config, notebook);
        if (err != VXCORE_OK) {
          VXCORE_LOG_ERROR("Failed to create bundled notebook: root_folder=%s, error=%d",
                           root_folder_clean.c_str(), err);
          return err;
        }
        break;
      }
      case NotebookType::Raw: {
        auto err = RawNotebook::Create(local_data_folder, root_folder_clean, &config, notebook);
        if (err != VXCORE_OK) {
          VXCORE_LOG_ERROR("Failed to create raw notebook: root_folder=%s, error=%d",
                           root_folder_clean.c_str(), err);
          return err;
        }
        break;
      }
      default:
        VXCORE_LOG_ERROR("Invalid notebook type: %d", static_cast<int>(type));
        return VXCORE_ERR_INVALID_PARAM;
    }

    auto err = UpdateNotebookRecord(*notebook);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Failed to save notebook record after creation: id=%s, error=%d",
                       notebook->GetId().c_str(), err);
      return err;
    }

    out_notebook_id = notebook->GetId();
    if (event_manager_ && notebook->GetFolderManager()) {
      notebook->GetFolderManager()->SetEventManager(event_manager_);
      notebook->SetEventManager(event_manager_);
    }
    notebooks_[out_notebook_id] = std::move(notebook);

    VXCORE_LOG_INFO("Notebook created successfully: id=%s", out_notebook_id.c_str());
    if (event_manager_) {
      event_manager_->Emit(events::kNotebookOpened, {{kJsonKeyNotebookId, out_notebook_id}});
    }
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    VXCORE_LOG_ERROR("JSON parse error while creating notebook: %s", e.what());
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::filesystem::filesystem_error &e) {
    VXCORE_LOG_ERROR("Filesystem error while creating notebook: %s", e.what());
    return VXCORE_ERR_IO;
  } catch (...) {
    VXCORE_LOG_ERROR("Unknown error while creating notebook");
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::OpenNotebook(const std::string &root_folder,
                                          std::string &out_notebook_id) {
  VXCORE_LOG_INFO("Opening notebook: root_folder=%s", root_folder.c_str());
  const auto root_folder_clean = CleanPath(root_folder);
  if (auto *notebook = FindNotebookByRootFolder(root_folder_clean)) {
    out_notebook_id = notebook->GetId();
    VXCORE_LOG_DEBUG("Notebook already open: id=%s", out_notebook_id.c_str());
    return VXCORE_OK;
  }

  auto rootPath = PathFromUtf8(root_folder_clean);
  if (!std::filesystem::exists(rootPath)) {
    VXCORE_LOG_WARN("Notebook root folder not found: %s", root_folder_clean.c_str());
    return VXCORE_ERR_NOT_FOUND;
  }

  std::unique_ptr<Notebook> notebook;
  auto err =
      BundledNotebook::Open(config_manager_->GetLocalDataPath(), root_folder_clean, notebook);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to load bundled notebook: root_folder=%s, error=%d",
                     root_folder_clean.c_str(), err);
    return err;
  }

  err = UpdateNotebookRecord(*notebook);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to save notebook record after open: id=%s, error=%d",
                     notebook->GetId().c_str(), err);
    return err;
  }

  out_notebook_id = notebook->GetId();
  if (event_manager_ && notebook->GetFolderManager()) {
    notebook->GetFolderManager()->SetEventManager(event_manager_);
    notebook->SetEventManager(event_manager_);
  }
  notebooks_[out_notebook_id] = std::move(notebook);

  VXCORE_LOG_INFO("Notebook open successfully: id=%s", out_notebook_id.c_str());
  if (event_manager_) {
    event_manager_->Emit(events::kNotebookOpened, {{kJsonKeyNotebookId, out_notebook_id}});
  }
  return VXCORE_OK;
}

VxCoreError NotebookManager::CloseNotebook(const std::string &notebook_id) {
  VXCORE_LOG_INFO("Closing notebook: id=%s", notebook_id.c_str());

  auto it = notebooks_.find(notebook_id);
  if (it == notebooks_.end()) {
    VXCORE_LOG_WARN("Notebook not found for closing: id=%s", notebook_id.c_str());
    return VXCORE_ERR_NOT_FOUND;
  }

  // Capture the cleaned root before the runtime entry is erased so the session
  // record can be matched by root even when its persisted id is stale.
  const std::string root_clean = CleanPath(it->second->GetRootFolder());

  // Close notebook first to release DB file lock before deleting local data
  it->second->Close();

  DeleteNotebookLocalData(*it->second);

  notebooks_.erase(it);

  // Remove notebook record(s) from session config: match by id OR cleaned root,
  // so a record with a stale id (diverged from config.json) is still removed.
  auto &session_config = config_manager_->GetSessionConfig();
  session_config.notebooks.erase(
      std::remove_if(session_config.notebooks.begin(), session_config.notebooks.end(),
                     [&notebook_id, &root_clean](const NotebookRecord &r) {
                       return r.id == notebook_id || CleanPath(r.root_folder) == root_clean;
                     }),
      session_config.notebooks.end());

  config_manager_->SaveSessionConfig();

  VXCORE_LOG_INFO("Notebook closed successfully: id=%s", notebook_id.c_str());
  if (event_manager_) {
    event_manager_->Emit(events::kNotebookClosed, {{kJsonKeyNotebookId, notebook_id}});
  }
  return VXCORE_OK;
}

void NotebookManager::DeleteNotebookLocalData(const Notebook &notebook) {
  const auto local_data_folder = notebook.GetLocalDataFolder();

  if (std::filesystem::exists(PathFromUtf8(local_data_folder))) {
    VXCORE_LOG_INFO("Deleting notebook local data: id=%s, path=%s", notebook.GetId().c_str(),
                    local_data_folder.c_str());
    std::error_code ec;
    std::filesystem::remove_all(PathFromUtf8(local_data_folder), ec);
    if (ec) {
      VXCORE_LOG_ERROR("Failed to delete notebook local data: id=%s, path=%s, error=%s",
                       notebook.GetId().c_str(), local_data_folder.c_str(), ec.message().c_str());
    } else {
      VXCORE_LOG_DEBUG("Notebook local data deleted: id=%s, path=%s", notebook.GetId().c_str(),
                       local_data_folder.c_str());
    }
  }
}

VxCoreError NotebookManager::GetNotebookConfig(const std::string &notebook_id,
                                               std::string &out_config_json) {
  auto *notebook = GetNotebook(notebook_id);
  if (!notebook) {
    return VXCORE_ERR_NOT_FOUND;
  }

  try {
    nlohmann::json json = ToNotebookConfig(*notebook);
    out_config_json = json.dump();
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

nlohmann::json NotebookManager::ToNotebookConfig(const Notebook &notebook) const {
  nlohmann::json json = notebook.GetConfig().ToJson();
  json[kJsonKeyRootFolder] = notebook.GetRootFolder();
  json[kJsonKeyType] = notebook.GetTypeStr();
  return json;
}

VxCoreError NotebookManager::UpdateNotebookConfig(const std::string &notebook_id,
                                                  const std::string &config_json) {
  auto *notebook = GetNotebook(notebook_id);
  if (!notebook) {
    return VXCORE_ERR_NOT_FOUND;
  }

  try {
    nlohmann::json json = nlohmann::json::parse(config_json);
    NotebookConfig config = NotebookConfig::FromJson(json);
    config.id = notebook_id;

    VxCoreError err = notebook->UpdateConfig(config);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Failed to update notebook config: id=%s, error=%d", notebook_id.c_str(),
                       err);
      return err;
    }

    err = UpdateNotebookRecord(*notebook);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Failed to update notebook record after config update: id=%s, error=%d",
                       notebook_id.c_str(), err);
      return err;
    }

    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::ListNotebooks(std::string &out_notebooks_json) {
  try {
    nlohmann::json jsonArray = nlohmann::json::array();

    for (const auto &pair : notebooks_) {
      nlohmann::json item = ToNotebookConfig(*pair.second);
      jsonArray.push_back(item);
    }

    out_notebooks_json = jsonArray.dump();
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

Notebook *NotebookManager::GetNotebook(const std::string &notebook_id) {
  auto it = notebooks_.find(notebook_id);
  if (it == notebooks_.end()) {
    return nullptr;
  }
  return it->second.get();
}

VxCoreError NotebookManager::UpdateNotebookRecord(const Notebook &notebook) {
  auto &session_config = config_manager_->GetSessionConfig();
  const std::string id = notebook.GetId();
  const std::string root_clean = CleanPath(notebook.GetRootFolder());

  // Remove every record that matches by id OR cleaned root, then upsert a single
  // canonical record. This guarantees exactly one record per (id, root) and heals
  // a pre-existing duplicate or id/root mismatch on the next write.
  session_config.notebooks.erase(
      std::remove_if(session_config.notebooks.begin(), session_config.notebooks.end(),
                     [&id, &root_clean](const NotebookRecord &r) {
                       return r.id == id || CleanPath(r.root_folder) == root_clean;
                     }),
      session_config.notebooks.end());

  session_config.notebooks.emplace_back();
  NotebookRecord *record = &session_config.notebooks.back();
  record->id = notebook.GetId();
  record->root_folder = notebook.GetRootFolder();
  record->type = notebook.GetType();
  // T14: persist the per-device read-only flag alongside the rest of the
  // record so subsequent session-restore (T15's LoadOpenNotebooks change)
  // can re-apply it via Notebook::SetReadOnly. Bundled and Raw notebooks
  // both participate; the runtime flag lives on the base class.
  record->read_only = notebook.IsReadOnly();

  config_manager_->SaveSessionConfig();
  return VXCORE_OK;
}

void NotebookManager::RecordNotebookReadOnly(const std::string &notebook_id, bool read_only) {
  // T14: best-effort persistence of the per-device RO flag. The runtime
  // flag has already been mutated on the Notebook by the caller via
  // SetReadOnly; this method just mirrors it into the persisted
  // NotebookRecord so the next session restore picks it up.
  auto *notebook = GetNotebook(notebook_id);
  if (!notebook) {
    VXCORE_LOG_WARN("RecordNotebookReadOnly: notebook not found id=%s", notebook_id.c_str());
    return;
  }
  // Defensive: callers should have already called SetReadOnly, but to make
  // this method usable from any path (T14 calls it after SetReadOnly; future
  // callers might pass through) we honor the explicit flag rather than
  // re-reading notebook->IsReadOnly().
  notebook->SetReadOnly(read_only);
  // UpdateNotebookRecord reads notebook->IsReadOnly() into record->read_only
  // and rewrites session.json. Failure (e.g. disk full) logs through the
  // SaveSessionConfig path -- we don't propagate because the runtime flag
  // is already correct and the caller (vxcore_notebook_open_ex) has already
  // succeeded; failing here would orphan the registered notebook.
  VxCoreError err = UpdateNotebookRecord(*notebook);
  if (err != VXCORE_OK) {
    VXCORE_LOG_WARN("RecordNotebookReadOnly: UpdateNotebookRecord failed for id=%s, err=%d",
                    notebook_id.c_str(), err);
  }
}

NotebookRecord *NotebookManager::FindNotebookRecord(const std::string &id) {
  auto &session_config = config_manager_->GetSessionConfig();
  auto it = std::find_if(session_config.notebooks.begin(), session_config.notebooks.end(),
                         [&id](const NotebookRecord &r) { return r.id == id; });

  if (it != session_config.notebooks.end()) {
    return &(*it);
  }

  return nullptr;
}

Notebook *NotebookManager::FindNotebookByRootFolder(const std::string &root_folder) {
  for (const auto &pair : notebooks_) {
    if (pair.second->GetRootFolder() == root_folder) {
      return pair.second.get();
    }
  }
  return nullptr;
}

VxCoreError NotebookManager::ResolvePathToNotebook(const std::string &absolute_path,
                                                   std::string &out_notebook_id,
                                                   std::string &out_relative_path) {
  const std::string clean_path = CleanPath(absolute_path);
  auto abs_path = PathFromUtf8(clean_path);

  // Iterate through all open notebooks and find which one contains this path.
  for (const auto &pair : notebooks_) {
    const std::string &root_folder = pair.second->GetRootFolder();
    auto root_path = PathFromUtf8(root_folder);

    // Check if absolute_path starts with root_folder.
    auto [root_end, path_pos] = std::mismatch(root_path.begin(), root_path.end(), abs_path.begin());

    if (root_end == root_path.end()) {
      // Path is within this notebook.
      out_notebook_id = pair.first;

      // Compute relative path.
      std::filesystem::path relative;
      for (; path_pos != abs_path.end(); ++path_pos) {
        relative /= *path_pos;
      }
      out_relative_path = CleanFsPath(relative);

      VXCORE_LOG_DEBUG("Resolved path %s to notebook %s, relative path %s", clean_path.c_str(),
                       out_notebook_id.c_str(), out_relative_path.c_str());
      return VXCORE_OK;
    }
  }

  VXCORE_LOG_DEBUG("Path %s not found in any open notebook", clean_path.c_str());
  return VXCORE_ERR_NOT_FOUND;
}

VxCoreError NotebookManager::ResolveNodeById(const std::string &node_id,
                                             std::string &out_notebook_id,
                                             std::string &out_relative_path) {
  for (const auto &pair : notebooks_) {
    auto *store = pair.second->GetMetadataStore();
    if (!store) {
      continue;
    }

    std::string path = store->GetNodePathById(node_id);
    if (!path.empty()) {
      out_notebook_id = pair.first;
      out_relative_path = path;
      VXCORE_LOG_DEBUG("Resolved node %s to notebook %s, relative path %s", node_id.c_str(),
                       out_notebook_id.c_str(), out_relative_path.c_str());
      return VXCORE_OK;
    }
  }

  VXCORE_LOG_DEBUG("Node %s not found in any open notebook", node_id.c_str());
  return VXCORE_ERR_NOT_FOUND;
}

void NotebookManager::SetEventManager(EventManager *event_manager) {
  event_manager_ = event_manager;
  // Propagate to existing notebooks and their folder managers
  for (auto &pair : notebooks_) {
    if (!pair.second) {
      continue;
    }
    pair.second->SetEventManager(event_manager_);
    if (auto *fm = pair.second->GetFolderManager()) {
      fm->SetEventManager(event_manager_);
    }
  }
}

}  // namespace vxcore
