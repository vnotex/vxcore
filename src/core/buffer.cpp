#include "buffer.h"

#include <vxcore/notebook_json_keys.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>

#include "buffer_provider.h"
#include "external_buffer_provider.h"
#include "notebook.h"
#include "standard_buffer_provider.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

namespace {
struct ProtectedContentWiper {
  std::vector<uint8_t> &content;
  ~ProtectedContentWiper() { NotebookEncryption::WipeBytes(content); }
};

template <typename Function>
VxCoreError ProtectedBufferOperation(Function &&function) {
  try {
    return function();
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

// Collapse CRLF and lone CR to a single LF so two byte streams that differ only
// in line-ending convention compare equal. Used by the external-change detector
// to tolerate line-ending-only rewrites from external editors. (VNote's git sync
// backend sets core.autocrlf=false, so sync itself does not rewrite EOLs, but
// third-party tools might.)
std::vector<uint8_t> NormalizeEol(const std::vector<uint8_t> &data) {
  std::vector<uint8_t> out;
  out.reserve(data.size());
  for (size_t i = 0; i < data.size(); ++i) {
    uint8_t c = data[i];
    if (c == '\r') {
      out.push_back('\n');
      if (i + 1 < data.size() && data[i + 1] == '\n') {
        ++i;  // Skip the LF of a CRLF pair.
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}
}  // namespace

Buffer::Buffer()
    : notebook_(nullptr),
      revision_(0),
      modified_(false),
      state_(VXCORE_BUFFER_NORMAL),
      last_modified_time_(0),
      content_loaded_(false) {
  id_ = GenerateUUID();
}

Buffer::Buffer(Notebook *notebook, const std::string &file_path)
    : notebook_(notebook),
      file_path_(file_path),
      revision_(0),
      modified_(false),
      state_(VXCORE_BUFFER_NORMAL),
      last_modified_time_(0),
      content_loaded_(false) {
  id_ = GenerateUUID();
  CreateProvider();
}

Buffer::Buffer(const std::string &absolute_path)
    : notebook_(nullptr),
      file_path_(absolute_path),
      revision_(0),
      modified_(false),
      state_(VXCORE_BUFFER_NORMAL),
      last_modified_time_(0),
      content_loaded_(false) {
  id_ = GenerateUUID();
  CreateProvider();
}

Buffer::Buffer(const std::string &address, bool virtual_flag)
    : notebook_(nullptr),
      file_path_(address),
      revision_(0),
      modified_(false),
      state_(VXCORE_BUFFER_NORMAL),
      last_modified_time_(0),
      content_loaded_(true),
      is_virtual_(virtual_flag) {
  id_ = GenerateUUID();
}

Buffer::~Buffer() {
  if (is_encrypted_) {
    NotebookEncryption::WipeBytes(content_);
  }
}

void Buffer::RefreshProtectionState() {
  if (discovered_encryption_) {
    return;
  }
  const bool encrypted = provider_ ? provider_->IsEncrypted()
                                  : file_path_.size() >= 4 &&
                                        file_path_.compare(file_path_.size() - 4, 4, ".vne") == 0;
  if (content_loaded_ && encrypted != is_encrypted_) {
    // Conversion replaces handles. A changed live classification must not turn
    // cached plaintext into an authenticated document or downgrade protected IO.
    discovered_encryption_ = true;
    is_encrypted_ = true;
    protection_error_ = VXCORE_ERR_ENCRYPTION_FORMAT;
    state_ = VXCORE_BUFFER_FILE_CHANGED;
    return;
  }
  is_encrypted_ = encrypted;
  protection_error_ = provider_ ? provider_->GetProtectionError()
                               : encrypted ? VXCORE_ERR_ENCRYPTION_FORMAT : VXCORE_OK;
  if (encrypted && (!notebook_ || notebook_->GetType() != NotebookType::Bundled)) {
    protection_error_ = VXCORE_ERR_UNSUPPORTED;
  }
}

StandardBufferProvider *Buffer::GetProtectedProvider(VxCoreError &out_error) const noexcept {
  out_error = protection_error_;
  if (out_error != VXCORE_OK) {
    return nullptr;
  }
  auto *provider = dynamic_cast<StandardBufferProvider *>(provider_.get());
  if (!provider || !provider->IsEncrypted()) {
    out_error = VXCORE_ERR_UNSUPPORTED;
    return nullptr;
  }
  out_error = provider->GetProtectionError();
  return out_error == VXCORE_OK ? provider : nullptr;
}

std::string Buffer::GetAuthenticatedEditorType() const {
  if (!is_encrypted_ || !content_loaded_) {
    return {};
  }
  VxCoreError error;
  auto *provider = GetProtectedProvider(error);
  return provider ? provider->GetAuthenticatedEditorType() : std::string();
}

bool Buffer::DetectEncryptedContent(const std::vector<uint8_t> &data) {
  if (data.size() < 8 ||
      (std::memcmp(data.data(), "VNOTEE1\0", 8) != 0 &&
       std::memcmp(data.data(), "VNEKEY1\0", 8) != 0)) {
    return false;
  }
  discovered_encryption_ = true;
  is_encrypted_ = true;
  protection_error_ = notebook_ && notebook_->GetType() == NotebookType::Bundled
                          ? VXCORE_ERR_ENCRYPTION_FORMAT : VXCORE_ERR_UNSUPPORTED;
  state_ = VXCORE_BUFFER_FILE_CHANGED;
  return true;
}

void Buffer::ReplaceProtectedContent(std::vector<uint8_t> &data) noexcept {
  if (&data != &content_) {
    NotebookEncryption::WipeBytes(content_);
    content_.swap(data);
  }
}

void Buffer::UpdateProtectedTimestamp(const std::string &full_path) noexcept {
  // Publication already succeeded. An unavailable timestamp must not turn a
  // durable protected save into a reported failure; fingerprints detect changes.
  try {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(PathFromUtf8(full_path), error);
    if (!error) {
      last_modified_time_ =
          std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    }
  } catch (...) {
  }
}

std::string Buffer::GetNotebookId() const { return notebook_ ? notebook_->GetId() : ""; }

std::string Buffer::ResolveFullPath() const {
  if (is_virtual_) {
    return file_path_;
  }

  if (!notebook_) {
    // External file - file_path is absolute
    return file_path_;
  }
  return ConcatenatePaths(notebook_->GetRootFolder(), file_path_);
}

std::string Buffer::GetBackupFilePath() {
  if (is_virtual_) {
    return "";
  }

  if (backup_file_path_.empty()) {
    // Use the UTF-8-safe CleanPath(std::string) rather than
    // CleanFsPath(std::string): the latter implicitly converts the narrow
    // std::string to std::filesystem::path using the active ANSI code page,
    // which throws "No mapping for the Unicode character exists in the target
    // multi-byte code page" on non-ASCII paths (CJK, full-width（）, etc.) on
    // Windows. That throw propagated out of the save/move path and surfaced as
    // a spurious "Failed to move" error while leaving assets behind (issue
    // #2721).
    backup_file_path_ = CleanPath(ResolveFullPath() + ".vswp");
  }
  return backup_file_path_;
}

VxCoreError Buffer::WriteBackup() {
  if (is_virtual_) {
    return VXCORE_OK;
  }
  if (notebook_ && notebook_->IsEncryptionRecoveryRequired()) {
    return VXCORE_ERR_ENCRYPTION_RECOVERY_REQUIRED;
  }

  if (!content_loaded_) {
    return is_encrypted_ ? (protection_error_ != VXCORE_OK ? protection_error_
                                                         : VXCORE_ERR_ENCRYPTION_LOCKED)
                         : VXCORE_ERR_INVALID_STATE;
  }

  if (is_encrypted_) {
    return ProtectedBufferOperation([&]() {
      VxCoreError error;
      auto *provider = GetProtectedProvider(error);
      if (!provider) {
        return error;
      }
      error = provider->WriteEncryptedBackup(content_, revision_);
      if (error == VXCORE_ERR_FILE_CHANGED_OUTSIDE) {
        state_ = VXCORE_BUFFER_FILE_CHANGED;
      }
      return error;
    });
  }

  try {
    const std::string backup_path = GetBackupFilePath();
    const auto backup_fs_path = PathFromUtf8(backup_path);
    const std::string header = "vnotex_backup_file " + ResolveFullPath() + "|";

    std::ofstream file(backup_fs_path, std::ios::binary);
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }

    if (!file.write(header.data(), static_cast<std::streamsize>(header.size()))) {
      return VXCORE_ERR_IO;
    }

    if (!content_.empty() && !file.write(reinterpret_cast<const char *>(content_.data()),
                                         static_cast<std::streamsize>(content_.size()))) {
      return VXCORE_ERR_IO;
    }

    file.close();
    if (file.fail()) {
      return VXCORE_ERR_IO;
    }

    VXCORE_LOG_DEBUG("Wrote backup file: %s", backup_path.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to write backup for %s: %s", ResolveFullPath().c_str(), e.what());
    return VXCORE_ERR_IO;
  }
}

bool Buffer::HasBackup() {
  if (is_virtual_) {
    return false;
  }

  return std::filesystem::exists(PathFromUtf8(GetBackupFilePath()));
}

VxCoreError Buffer::RecoverBackup() {
  if (is_virtual_) {
    return VXCORE_OK;
  }
  if (notebook_ && notebook_->IsEncryptionRecoveryRequired()) {
    return VXCORE_ERR_ENCRYPTION_RECOVERY_REQUIRED;
  }

  if (is_encrypted_) {
    return ProtectedBufferOperation([&]() {
      VxCoreError error;
      auto *provider = GetProtectedProvider(error);
      if (!provider) {
        return error;
      }
      const auto full_path = ResolveFullPath();
      std::vector<uint8_t> candidate;
      ProtectedContentWiper wipe{candidate};
      int recovered_revision = revision_;
      error = provider->RecoverEncryptedBackup(candidate, recovered_revision);
      if (error != VXCORE_OK) {
        if (error == VXCORE_ERR_FILE_CHANGED_OUTSIDE) {
          state_ = VXCORE_BUFFER_FILE_CHANGED;
        }
        return error;
      }
      // The provider authenticates and durably publishes before we adopt bytes.
      ReplaceProtectedContent(candidate);
      content_loaded_ = true;
      modified_ = false;
      revision_ = std::max(revision_, recovered_revision) + 2;
      state_ = VXCORE_BUFFER_NORMAL;
      UpdateProtectedTimestamp(full_path);
      return VXCORE_OK;
    });
  }

  if (!HasBackup()) {
    return VXCORE_ERR_NOT_FOUND;
  }

  try {
    const std::string backup_path = GetBackupFilePath();
    std::filesystem::path fs_path = PathFromUtf8(backup_path);

    // Read backup file content in a scoped block to ensure the file handle is
    // closed before we attempt to delete the backup file (required on Windows).
    std::vector<uint8_t> backup_content;
    {
      std::ifstream file(fs_path, std::ios::binary | std::ios::ate);
      if (!file.is_open()) {
        return VXCORE_ERR_IO;
      }

      std::streamsize size = file.tellg();
      if (size < 0) {
        return VXCORE_ERR_IO;
      }

      file.seekg(0, std::ios::beg);

      backup_content.resize(static_cast<size_t>(size));
      if (size > 0 && !file.read(reinterpret_cast<char *>(backup_content.data()), size)) {
        return VXCORE_ERR_IO;
      }
    }  // file handle closed here

    if (DetectEncryptedContent(backup_content)) {
      return protection_error_;
    }

    const auto separator_it =
        std::find(backup_content.begin(), backup_content.end(), static_cast<uint8_t>('|'));
    if (separator_it == backup_content.end()) {
      return VXCORE_ERR_IO;
    }

    content_.assign(separator_it + 1, backup_content.end());
    content_loaded_ = true;
    modified_ = true;
    revision_++;

    const auto error = SaveContent(ResolveFullPath());
    if (error != VXCORE_OK) {
      return error;
    }

    if (!std::filesystem::remove(PathFromUtf8(backup_path))) {
      return VXCORE_ERR_IO;
    }

    VXCORE_LOG_INFO("Recovered backup file: %s", backup_path.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to recover backup for %s: %s", ResolveFullPath().c_str(), e.what());
    return VXCORE_ERR_IO;
  }
}

void Buffer::DiscardBackup() {
  if (is_virtual_ || (notebook_ && notebook_->IsEncryptionRecoveryRequired())) {
    return;
  }

  try {
    const std::string backup_path = GetBackupFilePath();
    if (std::filesystem::exists(PathFromUtf8(backup_path)) &&
        std::filesystem::remove(PathFromUtf8(backup_path))) {
      VXCORE_LOG_DEBUG("Discarded backup file: %s", backup_path.c_str());
    }
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to discard backup for %s: %s", ResolveFullPath().c_str(), e.what());
  }
}

void Buffer::CreateProvider() {
  if (!notebook_) {
    // External file
    try {
      provider_ = std::make_unique<ExternalBufferProvider>(file_path_);
      VXCORE_LOG_DEBUG("Created ExternalBufferProvider for: %s", file_path_.c_str());
    } catch (const std::exception &e) {
      VXCORE_LOG_ERROR("Failed to create ExternalBufferProvider: %s", e.what());
    }
    RefreshProtectionState();
    return;
  }

  // Notebook file. Both bundled and raw notebooks support assets via the
  // StandardBufferProvider (generic FolderManager capabilities). Attachment
  // methods internally gate to bundled notebooks; raw stays UNSUPPORTED.
  try {
    provider_ = std::make_unique<StandardBufferProvider>(notebook_, file_path_);
    VXCORE_LOG_DEBUG("Created StandardBufferProvider for: %s", file_path_.c_str());
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to create StandardBufferProvider: %s", e.what());
  }
  RefreshProtectionState();
}

VxCoreError Buffer::LoadContent(const std::string &full_path) {
  if (is_virtual_) {
    return VXCORE_OK;
  }

  if (is_encrypted_) {
    return ProtectedBufferOperation([&]() {
      VxCoreError error;
      auto *provider = GetProtectedProvider(error);
      if (!provider) {
        return error;
      }
      std::vector<uint8_t> candidate;
      ProtectedContentWiper wipe{candidate};
      error = provider->LoadEncryptedContent(candidate);
      if (error != VXCORE_OK) {
        return error;
      }
      ReplaceProtectedContent(candidate);
      content_loaded_ = true;
      modified_ = false;
      revision_++;
      state_ = VXCORE_BUFFER_NORMAL;
      UpdateProtectedTimestamp(full_path);
      return VXCORE_OK;
    });
  }

  try {
    std::filesystem::path fs_path = PathFromUtf8(full_path);
    if (!std::filesystem::exists(fs_path)) {
      state_ = VXCORE_BUFFER_FILE_MISSING;
      VXCORE_LOG_ERROR("File not found: %s", full_path.c_str());
      return VXCORE_ERR_NOT_FOUND;
    }

    // Read file content as binary
    std::ifstream file(fs_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
      state_ = VXCORE_BUFFER_FILE_MISSING;
      VXCORE_LOG_ERROR("Failed to open file: %s", full_path.c_str());
      return VXCORE_ERR_IO;
    }

    std::streamsize size = file.tellg();
    if (size < 0) {
      state_ = VXCORE_BUFFER_FILE_MISSING;
      return VXCORE_ERR_IO;
    }
    file.seekg(0, std::ios::beg);

    content_.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char *>(content_.data()), size)) {
      state_ = VXCORE_BUFFER_FILE_MISSING;
      VXCORE_LOG_ERROR("Failed to read file: %s", full_path.c_str());
      content_.clear();
      return VXCORE_ERR_IO;
    }

    // Classification consumes only bytes obtained by the existing load.
    if (DetectEncryptedContent(content_)) {
      content_.clear();
      content_loaded_ = false;
      return protection_error_;
    }

    // Update last modified time
    auto ftime = std::filesystem::last_write_time(fs_path);
    last_modified_time_ =
        std::chrono::duration_cast<std::chrono::milliseconds>(ftime.time_since_epoch()).count();
    state_ = VXCORE_BUFFER_NORMAL;
    modified_ = false;
    content_loaded_ = true;
    revision_++;
    VXCORE_LOG_DEBUG("Loaded file content: %s (%zu bytes, revision %d)", full_path.c_str(),
                     content_.size(), revision_);
    return VXCORE_OK;
  } catch (const std::exception &e) {
    state_ = VXCORE_BUFFER_FILE_MISSING;
    VXCORE_LOG_ERROR("Exception loading file %s: %s", full_path.c_str(), e.what());
    content_.clear();
    return VXCORE_ERR_IO;
  }
}

VxCoreError Buffer::SaveContent(const std::string &full_path) {
  if (is_virtual_) {
    return VXCORE_OK;
  }
  if (notebook_ && notebook_->IsEncryptionRecoveryRequired()) {
    return VXCORE_ERR_ENCRYPTION_RECOVERY_REQUIRED;
  }

  if (is_encrypted_) {
    const auto error = ProtectedBufferOperation([&]() {
      VxCoreError result;
      auto *provider = GetProtectedProvider(result);
      if (!provider) {
        return result;
      }
      if (!content_loaded_) {
        return VXCORE_ERR_ENCRYPTION_LOCKED;
      }
      return provider->SaveEncryptedContent(content_);
    });
    if (error != VXCORE_OK) {
      state_ = error == VXCORE_ERR_FILE_CHANGED_OUTSIDE ? VXCORE_BUFFER_FILE_CHANGED
                                                      : VXCORE_BUFFER_SAVE_FAILED;
      return error;
    }
    state_ = VXCORE_BUFFER_NORMAL;
    modified_ = false;
    revision_++;
    UpdateProtectedTimestamp(full_path);
    return VXCORE_OK;
  }

  try {
    std::filesystem::path fs_path = PathFromUtf8(full_path);

    // Ensure parent directory exists
    std::filesystem::path parent = fs_path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
      std::filesystem::create_directories(parent);
    }

    // Write content as binary
    std::ofstream file(fs_path, std::ios::binary);
    if (!file.is_open()) {
      state_ = VXCORE_BUFFER_SAVE_FAILED;
      VXCORE_LOG_ERROR("Failed to open file for writing: %s", full_path.c_str());
      return VXCORE_ERR_IO;
    }

    if (!file.write(reinterpret_cast<const char *>(content_.data()), content_.size())) {
      state_ = VXCORE_BUFFER_SAVE_FAILED;
      VXCORE_LOG_ERROR("Failed to write file: %s", full_path.c_str());
      return VXCORE_ERR_IO;
    }

    file.close();
    if (file.fail()) {
      state_ = VXCORE_BUFFER_SAVE_FAILED;
      return VXCORE_ERR_IO;
    }

    // Update last modified time
    auto ftime = std::filesystem::last_write_time(fs_path);
    last_modified_time_ =
        std::chrono::duration_cast<std::chrono::milliseconds>(ftime.time_since_epoch()).count();

    state_ = VXCORE_BUFFER_NORMAL;
    modified_ = false;
    revision_++;
    VXCORE_LOG_DEBUG("Saved file content: %s (%zu bytes, revision %d)", full_path.c_str(),
                     content_.size(), revision_);
    return VXCORE_OK;
  } catch (const std::exception &e) {
    state_ = VXCORE_BUFFER_SAVE_FAILED;
    VXCORE_LOG_ERROR("Exception saving file %s: %s", full_path.c_str(), e.what());
    return VXCORE_ERR_IO;
  }
}

const std::vector<uint8_t> &Buffer::GetContent() const { return content_; }

void Buffer::SetContent(const std::vector<uint8_t> &data) {
  if (is_encrypted_) {
    if (&data != &content_) {
      // Copy before wiping: callers may pass GetContent() or otherwise borrow it.
      std::vector<uint8_t> candidate(data);
      ProtectedContentWiper wipe{candidate};
      ReplaceProtectedContent(candidate);
    }
  } else {
    content_ = data;
  }
  modified_ = true;
  content_loaded_ = true;
  revision_++;
  VXCORE_LOG_DEBUG("Content updated (revision %d, %zu bytes)", revision_, content_.size());
}

VxCoreError Buffer::CheckExternalChanges(const std::string &full_path) {
  if (is_virtual_) {
    return VXCORE_OK;
  }

  // Skip check for buffers that haven't loaded content yet (e.g., session-restored inactive tabs).
  // Their last_modified_time_ is 0 (constructor default), so any real mtime would differ,
  // causing a false positive FILE_CHANGED state.
  if (!content_loaded_ && last_modified_time_ == 0) {
    return VXCORE_OK;
  }

  if (is_encrypted_) {
    return ProtectedBufferOperation([&]() {
      VxCoreError error;
      auto *provider = GetProtectedProvider(error);
      if (!provider) {
        return error;
      }
      bool matches = false;
      error = provider->CheckEncryptedSnapshot(matches);
      if (error != VXCORE_OK) {
        state_ = error == VXCORE_ERR_NODE_NOT_EXISTS || error == VXCORE_ERR_NOT_FOUND
                     ? VXCORE_BUFFER_FILE_MISSING : VXCORE_BUFFER_FILE_CHANGED;
        return error;
      }
      if (!matches) {
        state_ = VXCORE_BUFFER_FILE_CHANGED;
      } else if (state_ == VXCORE_BUFFER_FILE_CHANGED || state_ == VXCORE_BUFFER_FILE_MISSING) {
        state_ = VXCORE_BUFFER_NORMAL;
      }
      return VXCORE_OK;
    });
  }

  try {
    std::filesystem::path fs_path = PathFromUtf8(full_path);
    if (!std::filesystem::exists(fs_path)) {
      if (state_ != VXCORE_BUFFER_FILE_MISSING) {
        state_ = VXCORE_BUFFER_FILE_MISSING;
        VXCORE_LOG_WARN("File no longer exists: %s", full_path.c_str());
      }
      return VXCORE_OK;
    }

    // Get current file modification time
    auto ftime = std::filesystem::last_write_time(fs_path);
    int64_t current_mtime =
        std::chrono::duration_cast<std::chrono::milliseconds>(ftime.time_since_epoch()).count();

    if (current_mtime == last_modified_time_) {
      // mtime matches our last known stamp: nothing changed on disk.
      if (state_ == VXCORE_BUFFER_FILE_CHANGED || state_ == VXCORE_BUFFER_FILE_MISSING) {
        state_ = VXCORE_BUFFER_NORMAL;  // File restored to normal state.
      }
      return VXCORE_OK;
    }

    // mtime differs from our stamp. A bare mtime bump is frequently benign and
    // must NOT, by itself, be reported as an external modification. Common benign
    // sources: VNote's own worker-thread save re-stamp racing this check, the git
    // sync backend rewriting the working tree with byte-identical content during
    // checkout/rebase, Windows lazy metadata flush, antivirus / cloud-sync touches.
    // Only a real CONTENT difference is a true external edit, so confirm by
    // comparing the on-disk bytes against our in-memory content_ before flagging.
    //
    // Thread-safety: the consumer (Qt-side BufferService) gates this check so it
    // never runs while a save worker mutates content_ for this buffer; content_
    // is therefore stable for the duration of the comparison below.
    if (FileContentMatchesBuffer(full_path)) {
      // Benign mtime change: refresh the stamp and stay NORMAL. The stamp is
      // refreshed ONLY on confirmed content equality, so a real, still-unresolved
      // external edit keeps flagging on every subsequent check until resolved.
      last_modified_time_ = current_mtime;
      if (state_ == VXCORE_BUFFER_FILE_CHANGED || state_ == VXCORE_BUFFER_FILE_MISSING) {
        state_ = VXCORE_BUFFER_NORMAL;
      }
      return VXCORE_OK;
    }

    // Real external modification: content differs. Leave last_modified_time_
    // UNTOUCHED so repeated checks keep flagging until the user resolves it.
    state_ = VXCORE_BUFFER_FILE_CHANGED;
    VXCORE_LOG_WARN("File changed externally: %s", full_path.c_str());
    return is_encrypted_ ? protection_error_ : VXCORE_OK;
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Exception checking file changes for %s: %s", full_path.c_str(), e.what());
    return VXCORE_ERR_IO;
  }
}

bool Buffer::FileContentMatchesBuffer(const std::string &full_path) {
  std::filesystem::path fs_path = PathFromUtf8(full_path);
  std::ifstream file(fs_path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return false;  // Unreadable → treat as differing (caller will flag CHANGED).
  }

  std::streamsize size = file.tellg();
  if (size < 0) {
    return false;
  }
  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> disk(static_cast<size_t>(size));
  if (size > 0 && !file.read(reinterpret_cast<char *>(disk.data()), size)) {
    return false;
  }

  if (DetectEncryptedContent(disk)) {
    return false;
  }

  // Fast path: exact byte equality. Covers identical-content rewrites (git
  // checkout, self-save re-stamp race, lazy mtime flush) without normalization.
  if (disk.size() == content_.size() && std::equal(disk.begin(), disk.end(), content_.begin())) {
    return true;
  }

  // Slow path: EOL-insensitive comparison so a line-ending-only rewrite by an
  // external tool is treated as benign.
  return NormalizeEol(disk) == NormalizeEol(content_);
}

BufferRecord::BufferRecord() {}

BufferRecord BufferRecord::FromJson(const nlohmann::json &json) {
  BufferRecord record;
  if (json.contains(kJsonKeyId) && json[kJsonKeyId].is_string()) {
    record.id = json[kJsonKeyId].get<std::string>();
  }
  if (json.contains(kJsonKeyNotebookId) && json[kJsonKeyNotebookId].is_string()) {
    record.notebook_id = json[kJsonKeyNotebookId].get<std::string>();
  }
  if (json.contains("filePath") && json["filePath"].is_string()) {
    record.file_path = json["filePath"].get<std::string>();
  }
  return record;
}

nlohmann::json BufferRecord::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  json[kJsonKeyId] = id;
  json[kJsonKeyNotebookId] = notebook_id;
  json["filePath"] = file_path;
  return json;
}

}  // namespace vxcore
