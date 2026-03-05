#ifndef VXCORE_BUFFER_H
#define VXCORE_BUFFER_H

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "vxcore/vxcore_types.h"

namespace vxcore {

struct Buffer {
  std::string id;
  std::string notebook_id;  // Empty for external files
  std::string file_path;    // Absolute if notebook_id empty, else relative
  int revision;             // Content revision number
  bool modified;            // Unsaved changes flag
  VxCoreBufferState state;  // Buffer state (NORMAL, FILE_MISSING, etc.)
  nlohmann::json metadata;
  std::vector<uint8_t> content;  // Cached file content as raw bytes
  int64_t last_modified_time;    // File timestamp for change detection
  bool content_loaded;           // True if content has been loaded from disk

  Buffer();

  static Buffer FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;

  // Content management methods
  void LoadContent(const std::string &full_path);
  void SaveContent(const std::string &full_path);
  const std::vector<uint8_t> &GetContent() const;
  void SetContent(const std::vector<uint8_t> &data);
  void CheckExternalChanges(const std::string &full_path);
};

struct BufferRecord {
  std::string id;
  std::string notebook_id;
  std::string file_path;

  BufferRecord();

  static BufferRecord FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
};

}  // namespace vxcore

#endif  // VXCORE_BUFFER_H
