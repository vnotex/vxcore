#include "buffer.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "buffer_provider.h"
#include "external_buffer_provider.h"
#include "notebook.h"
#include "standard_buffer_provider.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

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

Buffer::~Buffer() = default;

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
    backup_file_path_ = CleanFsPath(ResolveFullPath() + ".vswp");
  }
  return backup_file_path_;
}

VxCoreError Buffer::WriteBackup() {
  if (is_virtual_) {
    return VXCORE_OK;
  }

  if (!content_loaded_) {
    return VXCORE_ERR_INVALID_STATE;
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

    const auto separator_it =
        std::find(backup_content.begin(), backup_content.end(), static_cast<uint8_t>('|'));
    if (separator_it == backup_content.end()) {
      return VXCORE_ERR_IO;
    }

    content_.assign(separator_it + 1, backup_content.end());
    content_loaded_ = true;
    modified_ = true;
    revision_++;

    SaveContent(ResolveFullPath());
    if (state_ == VXCORE_BUFFER_SAVE_FAILED) {
      return VXCORE_ERR_IO;
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
  if (is_virtual_) {
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
    return;
  }

  // Notebook file - only bundled notebooks support provider
  if (notebook_->GetTypeStr() != "bundled") {
    VXCORE_LOG_DEBUG("Notebook type '%s' does not support buffer provider",
                     notebook_->GetTypeStr().c_str());
    return;
  }

  try {
    provider_ = std::make_unique<StandardBufferProvider>(notebook_, file_path_);
    VXCORE_LOG_DEBUG("Created StandardBufferProvider for: %s", file_path_.c_str());
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to create StandardBufferProvider: %s", e.what());
  }
}

void Buffer::LoadContent(const std::string &full_path) {
  if (is_virtual_) {
    return;
  }

  try {
    std::filesystem::path fs_path = PathFromUtf8(full_path);
    if (!std::filesystem::exists(fs_path)) {
      state_ = VXCORE_BUFFER_FILE_MISSING;
      VXCORE_LOG_ERROR("File not found: %s", full_path.c_str());
      return;
    }

    // Read file content as binary
    std::ifstream file(fs_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
      state_ = VXCORE_BUFFER_FILE_MISSING;
      VXCORE_LOG_ERROR("Failed to open file: %s", full_path.c_str());
      return;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    content_.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char *>(content_.data()), size)) {
      state_ = VXCORE_BUFFER_FILE_MISSING;
      VXCORE_LOG_ERROR("Failed to read file: %s", full_path.c_str());
      content_.clear();
      return;
    }

    // Update last modified time
    auto ftime = std::filesystem::last_write_time(fs_path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    last_modified_time_ =
        std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch()).count();

    state_ = VXCORE_BUFFER_NORMAL;
    modified_ = false;
    content_loaded_ = true;
    VXCORE_LOG_DEBUG("Loaded file content: %s (%zu bytes)", full_path.c_str(), content_.size());
  } catch (const std::exception &e) {
    state_ = VXCORE_BUFFER_FILE_MISSING;
    VXCORE_LOG_ERROR("Exception loading file %s: %s", full_path.c_str(), e.what());
    content_.clear();
  }
}

void Buffer::SaveContent(const std::string &full_path) {
  if (is_virtual_) {
    return;
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
      return;
    }

    if (!file.write(reinterpret_cast<const char *>(content_.data()), content_.size())) {
      state_ = VXCORE_BUFFER_SAVE_FAILED;
      VXCORE_LOG_ERROR("Failed to write file: %s", full_path.c_str());
      return;
    }

    file.close();

    // Update last modified time
    auto ftime = std::filesystem::last_write_time(fs_path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    last_modified_time_ =
        std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch()).count();

    state_ = VXCORE_BUFFER_NORMAL;
    modified_ = false;
    revision_++;
    VXCORE_LOG_DEBUG("Saved file content: %s (%zu bytes, revision %d)", full_path.c_str(),
                     content_.size(), revision_);
  } catch (const std::exception &e) {
    state_ = VXCORE_BUFFER_SAVE_FAILED;
    VXCORE_LOG_ERROR("Exception saving file %s: %s", full_path.c_str(), e.what());
  }
}

const std::vector<uint8_t> &Buffer::GetContent() const { return content_; }

void Buffer::SetContent(const std::vector<uint8_t> &data) {
  content_ = data;
  modified_ = true;
  content_loaded_ = true;
  revision_++;
  VXCORE_LOG_DEBUG("Content updated (revision %d, %zu bytes)", revision_, content_.size());
}

void Buffer::CheckExternalChanges(const std::string &full_path) {
  if (is_virtual_) {
    return;
  }

  try {
    std::filesystem::path fs_path = PathFromUtf8(full_path);
    if (!std::filesystem::exists(fs_path)) {
      if (state_ != VXCORE_BUFFER_FILE_MISSING) {
        state_ = VXCORE_BUFFER_FILE_MISSING;
        VXCORE_LOG_WARN("File no longer exists: %s", full_path.c_str());
      }
      return;
    }

    // Get current file modification time
    auto ftime = std::filesystem::last_write_time(fs_path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    int64_t current_mtime =
        std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch()).count();

    // Compare with stored modification time
    if (current_mtime != last_modified_time_) {
      state_ = VXCORE_BUFFER_FILE_CHANGED;
      VXCORE_LOG_WARN("File changed externally: %s", full_path.c_str());
    } else if (state_ == VXCORE_BUFFER_FILE_CHANGED || state_ == VXCORE_BUFFER_FILE_MISSING) {
      // File restored to normal state
      state_ = VXCORE_BUFFER_NORMAL;
    }
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Exception checking file changes for %s: %s", full_path.c_str(), e.what());
  }
}

BufferRecord::BufferRecord() {}

BufferRecord BufferRecord::FromJson(const nlohmann::json &json) {
  BufferRecord record;
  if (json.contains("id") && json["id"].is_string()) {
    record.id = json["id"].get<std::string>();
  }
  if (json.contains("notebookId") && json["notebookId"].is_string()) {
    record.notebook_id = json["notebookId"].get<std::string>();
  }
  if (json.contains("filePath") && json["filePath"].is_string()) {
    record.file_path = json["filePath"].get<std::string>();
  }
  return record;
}

nlohmann::json BufferRecord::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  json["id"] = id;
  json["notebookId"] = notebook_id;
  json["filePath"] = file_path;
  return json;
}

}  // namespace vxcore
