#include "buffer_manager.h"

#include <algorithm>

#include "config_manager.h"
#include "notebook.h"
#include "notebook_manager.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

BufferManager::BufferManager(ConfigManager *config_manager, NotebookManager *notebook_manager)
    : config_manager_(config_manager), notebook_manager_(notebook_manager) {
  LoadBuffers();
}

BufferManager::~BufferManager() { SaveBuffers(); }

void BufferManager::LoadBuffers() {
  auto &session_config = config_manager_->GetSessionConfig();
  buffers_.clear();

  for (const auto &record : session_config.buffers) {
    auto buffer = std::make_unique<Buffer>();
    buffer->id = record.id;
    buffer->notebook_id = record.notebook_id;
    buffer->file_path = record.file_path;
    buffers_[buffer->id] = std::move(buffer);
    VXCORE_LOG_DEBUG("Loaded buffer: id=%s, notebook_id=%s, file_path=%s", record.id.c_str(),
                     record.notebook_id.c_str(), record.file_path.c_str());
  }

  VXCORE_LOG_INFO("Loaded %zu buffers from session", buffers_.size());
}

void BufferManager::SaveBuffers() {
  auto &session_config = config_manager_->GetSessionConfig();
  session_config.buffers.clear();

  for (const auto &pair : buffers_) {
    BufferRecord record;
    record.id = pair.second->id;
    record.notebook_id = pair.second->notebook_id;
    record.file_path = pair.second->file_path;
    session_config.buffers.push_back(record);
  }

  config_manager_->SaveSessionConfig();
  VXCORE_LOG_DEBUG("Saved %zu buffers to session", buffers_.size());
}

std::string BufferManager::ResolveFullPath(const std::string &notebook_id,
                                           const std::string &file_path) {
  if (notebook_id.empty()) {
    // External file - file_path is absolute
    return file_path;
  }

  // Notebook file - resolve relative to notebook root
  auto *notebook = notebook_manager_->GetNotebook(notebook_id);
  if (!notebook) {
    VXCORE_LOG_ERROR("Cannot resolve path: notebook not found: %s", notebook_id.c_str());
    return "";
  }

  return ConcatenatePaths(notebook->GetRootFolder(), file_path);
}

std::string BufferManager::FindBufferByPath(const std::string &notebook_id,
                                            const std::string &file_path) {
  for (const auto &pair : buffers_) {
    const auto &buffer = pair.second;
    if (buffer->notebook_id == notebook_id && buffer->file_path == file_path) {
      return buffer->id;
    }
  }
  return "";
}

std::string BufferManager::OpenBuffer(const std::string &notebook_id,
                                      const std::string &file_path) {
  // Check if buffer already exists (de-duplication)
  std::string existing_id = FindBufferByPath(notebook_id, file_path);
  if (!existing_id.empty()) {
    VXCORE_LOG_DEBUG("Buffer already open: id=%s, path=%s", existing_id.c_str(), file_path.c_str());
    return existing_id;
  }

  // Create new buffer (content loaded lazily on first access)
  auto buffer = std::make_unique<Buffer>();
  buffer->id = GenerateUUID();
  buffer->notebook_id = notebook_id;
  buffer->file_path = file_path;

  std::string id = buffer->id;
  buffers_[id] = std::move(buffer);

  SaveBuffers();
  VXCORE_LOG_INFO("Opened buffer: id=%s, notebook_id=%s, file_path=%s", id.c_str(),
                  notebook_id.c_str(), file_path.c_str());
  return id;
}

bool BufferManager::CloseBuffer(const std::string &id) {
  auto it = buffers_.find(id);
  if (it == buffers_.end()) {
    VXCORE_LOG_WARN("Cannot close non-existent buffer: id=%s", id.c_str());
    return false;
  }

  buffers_.erase(it);
  SaveBuffers();
  VXCORE_LOG_INFO("Closed buffer: id=%s", id.c_str());
  return true;
}

Buffer *BufferManager::GetBuffer(const std::string &id) {
  auto it = buffers_.find(id);
  if (it == buffers_.end()) {
    return nullptr;
  }
  return it->second.get();
}

std::vector<Buffer> BufferManager::ListBuffers() {
  std::vector<Buffer> result;
  result.reserve(buffers_.size());
  for (const auto &pair : buffers_) {
    result.push_back(*pair.second);
  }
  return result;
}

VxCoreError BufferManager::SaveBuffer(const std::string &id) {
  auto *buffer = GetBuffer(id);
  if (!buffer) {
    VXCORE_LOG_ERROR("Cannot save: buffer not found: id=%s", id.c_str());
    return VXCORE_ERR_BUFFER_NOT_FOUND;
  }

  // Resolve full path for saving
  std::string full_path = ResolveFullPath(buffer->notebook_id, buffer->file_path);
  if (full_path.empty()) {
    VXCORE_LOG_ERROR("Cannot save: failed to resolve path for buffer: id=%s", id.c_str());
    return VXCORE_ERR_IO;
  }
  buffer->SaveContent(full_path);

  // Check if save succeeded
  if (buffer->state == VXCORE_BUFFER_SAVE_FAILED) {
    return VXCORE_ERR_IO;
  }

  VXCORE_LOG_INFO("Saved buffer: id=%s, revision=%d", id.c_str(), buffer->revision);
  return VXCORE_OK;
}

VxCoreError BufferManager::ReloadBuffer(const std::string &id) {
  auto *buffer = GetBuffer(id);
  if (!buffer) {
    VXCORE_LOG_ERROR("Cannot reload: buffer not found: id=%s", id.c_str());
    return VXCORE_ERR_BUFFER_NOT_FOUND;
  }

  // Resolve full path for loading
  std::string full_path = ResolveFullPath(buffer->notebook_id, buffer->file_path);
  if (full_path.empty()) {
    VXCORE_LOG_ERROR("Cannot reload: failed to resolve path for buffer: id=%s", id.c_str());
    return VXCORE_ERR_NOT_FOUND;
  }
  buffer->LoadContent(full_path);

  // Check if load succeeded
  if (buffer->state == VXCORE_BUFFER_FILE_MISSING) {
    return VXCORE_ERR_NOT_FOUND;
  }

  VXCORE_LOG_INFO("Reloaded buffer: id=%s, %zu bytes", id.c_str(), buffer->content.size());
  return VXCORE_OK;
}

VxCoreError BufferManager::GetBufferContent(const std::string &id, const void **out_data,
                                            size_t *out_size) {
  if (!out_data || !out_size) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *buffer = GetBuffer(id);
  if (!buffer) {
    VXCORE_LOG_ERROR("Cannot get content: buffer not found: id=%s", id.c_str());
    return VXCORE_ERR_BUFFER_NOT_FOUND;
  }

  // Lazy load content if not yet loaded
  if (!buffer->content_loaded) {
    std::string full_path = ResolveFullPath(buffer->notebook_id, buffer->file_path);
    if (full_path.empty()) {
      VXCORE_LOG_ERROR("Cannot load content: failed to resolve path for buffer: id=%s", id.c_str());
      return VXCORE_ERR_NOT_FOUND;
    }
    buffer->LoadContent(full_path);
    if (buffer->state == VXCORE_BUFFER_FILE_MISSING) {
      return VXCORE_ERR_NOT_FOUND;
    }
  }

  *out_data = buffer->content.data();
  *out_size = buffer->content.size();
  return VXCORE_OK;
}

VxCoreError BufferManager::SetBufferContent(const std::string &id, const void *data, size_t size) {
  if (!data) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *buffer = GetBuffer(id);
  if (!buffer) {
    VXCORE_LOG_ERROR("Cannot set content: buffer not found: id=%s", id.c_str());
    return VXCORE_ERR_BUFFER_NOT_FOUND;
  }

  // Copy data into buffer's content vector
  std::vector<uint8_t> new_content(size);
  std::memcpy(new_content.data(), data, size);

  buffer->SetContent(new_content);
  VXCORE_LOG_DEBUG("Set buffer content: id=%s, %zu bytes", id.c_str(), size);
  return VXCORE_OK;
}

void BufferManager::AutoSaveTick() {
  int64_t current_time = GetCurrentTimestampMillis();
  int saved_count = 0;

  for (const auto &pair : buffers_) {
    auto *buffer = pair.second.get();
    if (!buffer->modified) {
      continue;
    }

    // Check if enough time has passed since last modification
    int64_t time_since_modification = current_time - buffer->last_modified_time;
    if (time_since_modification >= auto_save_interval_ms_) {
      VxCoreError err = SaveBuffer(buffer->id);
      if (err == VXCORE_OK) {
        saved_count++;
      } else {
        VXCORE_LOG_WARN("Auto-save failed for buffer: id=%s, error=%d", buffer->id.c_str(), err);
      }
    }
  }

  if (saved_count > 0) {
    VXCORE_LOG_DEBUG("Auto-saved %d buffers", saved_count);
  }
}

void BufferManager::CloseBuffersForNotebook(const std::string &notebook_id) {
  std::vector<std::string> to_close;

  // Collect buffer IDs to close
  for (const auto &pair : buffers_) {
    if (pair.second->notebook_id == notebook_id) {
      to_close.push_back(pair.first);
    }
  }

  // Close collected buffers
  for (const auto &id : to_close) {
    CloseBuffer(id);
  }

  if (!to_close.empty()) {
    VXCORE_LOG_INFO("Closed %zu buffers for notebook: id=%s", to_close.size(), notebook_id.c_str());
  }
}

}  // namespace vxcore
