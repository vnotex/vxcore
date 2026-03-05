#ifndef VXCORE_BUFFER_MANAGER_H
#define VXCORE_BUFFER_MANAGER_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "buffer.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class ConfigManager;
class NotebookManager;

class BufferManager {
 public:
  BufferManager(ConfigManager *config_manager, NotebookManager *notebook_manager);
  ~BufferManager();

  // Open a buffer for a file, returns buffer ID (or existing ID if already open)
  std::string OpenBuffer(const std::string &notebook_id, const std::string &file_path);

  // Close a buffer and free its content
  bool CloseBuffer(const std::string &id);

  // Get buffer by ID, returns nullptr if not found
  Buffer *GetBuffer(const std::string &id);

  // List all open buffers
  std::vector<Buffer> ListBuffers();

  // Find buffer by path for de-duplication (returns buffer ID or empty string)
  std::string FindBufferByPath(const std::string &notebook_id, const std::string &file_path);

  // Save buffer to disk
  VxCoreError SaveBuffer(const std::string &id);

  // Reload buffer from disk
  VxCoreError ReloadBuffer(const std::string &id);

  // Get buffer content as raw memory (direct access for large files)
  VxCoreError GetBufferContent(const std::string &id, const void **out_data, size_t *out_size);

  // Set buffer content from raw memory
  VxCoreError SetBufferContent(const std::string &id, const void *data, size_t size);

  // Auto-save tick: save modified buffers that exceed auto-save interval
  void AutoSaveTick();

  // Close all buffers associated with a notebook
  void CloseBuffersForNotebook(const std::string &notebook_id);

  // Get/set auto-save interval (milliseconds)
  int64_t GetAutoSaveInterval() const { return auto_save_interval_ms_; }
  void SetAutoSaveInterval(int64_t interval_ms) { auto_save_interval_ms_ = interval_ms; }

 private:
  void LoadBuffers();
  void SaveBuffers();
  std::string ResolveFullPath(const std::string &notebook_id, const std::string &file_path);

  ConfigManager *config_manager_ = nullptr;
  NotebookManager *notebook_manager_ = nullptr;
  std::map<std::string, std::unique_ptr<Buffer>> buffers_;
  int64_t auto_save_interval_ms_ = 30000;  // Default 30 seconds
};

}  // namespace vxcore

#endif  // VXCORE_BUFFER_MANAGER_H
