#include "buffer.h"

#include <chrono>
#include <filesystem>
#include <fstream>

#include "utils/file_utils.h"
#include "utils/logger.h"

namespace vxcore {

Buffer::Buffer()
    : revision(0),
      modified(false),
      state(VXCORE_BUFFER_NORMAL),
      last_modified_time(0),
      content_loaded(false) {}

Buffer Buffer::FromJson(const nlohmann::json &json) {
  Buffer config;
  if (json.contains("id") && json["id"].is_string()) {
    config.id = json["id"].get<std::string>();
  }
  if (json.contains("notebookId") && json["notebookId"].is_string()) {
    config.notebook_id = json["notebookId"].get<std::string>();
  }
  if (json.contains("filePath") && json["filePath"].is_string()) {
    config.file_path = json["filePath"].get<std::string>();
  }
  if (json.contains("revision") && json["revision"].is_number_integer()) {
    config.revision = json["revision"].get<int>();
  }
  if (json.contains("modified") && json["modified"].is_boolean()) {
    config.modified = json["modified"].get<bool>();
  }
  if (json.contains("state") && json["state"].is_number_integer()) {
    config.state = static_cast<VxCoreBufferState>(json["state"].get<int>());
  }
  if (json.contains("metadata") && json["metadata"].is_object()) {
    config.metadata = json["metadata"];
  }
  if (json.contains("content") && json["content"].is_array()) {
    config.content = json["content"].get<std::vector<uint8_t>>();
  }
  if (json.contains("lastModifiedTime") && json["lastModifiedTime"].is_number_integer()) {
    config.last_modified_time = json["lastModifiedTime"].get<int64_t>();
  }
  if (json.contains("contentLoaded") && json["contentLoaded"].is_boolean()) {
    config.content_loaded = json["contentLoaded"].get<bool>();
  }
  return config;
}

nlohmann::json Buffer::ToJson() const {
  nlohmann::json json = nlohmann::json::object();
  json["id"] = id;
  json["notebookId"] = notebook_id;
  json["filePath"] = file_path;
  json["revision"] = revision;
  json["modified"] = modified;
  json["state"] = static_cast<int>(state);
  json["metadata"] = metadata;
  json["content"] = content;
  json["lastModifiedTime"] = last_modified_time;
  json["contentLoaded"] = content_loaded;
  return json;
}

void Buffer::LoadContent(const std::string &full_path) {
  try {
    std::filesystem::path fs_path(full_path);
    if (!std::filesystem::exists(fs_path)) {
      state = VXCORE_BUFFER_FILE_MISSING;
      VXCORE_LOG_ERROR("File not found: %s", full_path.c_str());
      return;
    }

    // Read file content as binary
    std::ifstream file(fs_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
      state = VXCORE_BUFFER_FILE_MISSING;
      VXCORE_LOG_ERROR("Failed to open file: %s", full_path.c_str());
      return;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    content.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char *>(content.data()), size)) {
      state = VXCORE_BUFFER_FILE_MISSING;
      VXCORE_LOG_ERROR("Failed to read file: %s", full_path.c_str());
      content.clear();
      return;
    }

    // Update last modified time
    auto ftime = std::filesystem::last_write_time(fs_path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    last_modified_time =
        std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch()).count();

    state = VXCORE_BUFFER_NORMAL;
    modified = false;
    content_loaded = true;
    VXCORE_LOG_DEBUG("Loaded file content: %s (%zu bytes)", full_path.c_str(), content.size());
  } catch (const std::exception &e) {
    state = VXCORE_BUFFER_FILE_MISSING;
    VXCORE_LOG_ERROR("Exception loading file %s: %s", full_path.c_str(), e.what());
    content.clear();
  }
}

void Buffer::SaveContent(const std::string &full_path) {
  try {
    std::filesystem::path fs_path(full_path);

    // Ensure parent directory exists
    std::filesystem::path parent = fs_path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
      std::filesystem::create_directories(parent);
    }

    // Write content as binary
    std::ofstream file(fs_path, std::ios::binary);
    if (!file.is_open()) {
      state = VXCORE_BUFFER_SAVE_FAILED;
      VXCORE_LOG_ERROR("Failed to open file for writing: %s", full_path.c_str());
      return;
    }

    if (!file.write(reinterpret_cast<const char *>(content.data()), content.size())) {
      state = VXCORE_BUFFER_SAVE_FAILED;
      VXCORE_LOG_ERROR("Failed to write file: %s", full_path.c_str());
      return;
    }

    file.close();

    // Update last modified time
    auto ftime = std::filesystem::last_write_time(fs_path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    last_modified_time =
        std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch()).count();

    state = VXCORE_BUFFER_NORMAL;
    modified = false;
    revision++;
    VXCORE_LOG_DEBUG("Saved file content: %s (%zu bytes, revision %d)", full_path.c_str(),
                     content.size(), revision);
  } catch (const std::exception &e) {
    state = VXCORE_BUFFER_SAVE_FAILED;
    VXCORE_LOG_ERROR("Exception saving file %s: %s", full_path.c_str(), e.what());
  }
}

const std::vector<uint8_t> &Buffer::GetContent() const { return content; }

void Buffer::SetContent(const std::vector<uint8_t> &data) {
  content = data;
  modified = true;
  content_loaded = true;
  revision++;
  VXCORE_LOG_DEBUG("Content updated (revision %d, %zu bytes)", revision, content.size());
}

void Buffer::CheckExternalChanges(const std::string &full_path) {
  try {
    std::filesystem::path fs_path(full_path);
    if (!std::filesystem::exists(fs_path)) {
      if (state != VXCORE_BUFFER_FILE_MISSING) {
        state = VXCORE_BUFFER_FILE_MISSING;
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
    if (current_mtime != last_modified_time) {
      state = VXCORE_BUFFER_FILE_CHANGED;
      VXCORE_LOG_WARN("File changed externally: %s", full_path.c_str());
    } else if (state == VXCORE_BUFFER_FILE_CHANGED || state == VXCORE_BUFFER_FILE_MISSING) {
      // File restored to normal state
      state = VXCORE_BUFFER_NORMAL;
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
