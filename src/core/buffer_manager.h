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
class IBufferProvider;
class NotebookManager;

class BufferManager {
 public:
  BufferManager(ConfigManager *config_manager, NotebookManager *notebook_manager);
  ~BufferManager();

  // Open a buffer for a file, returns buffer ID (or existing ID if already open)
  // For notebook files: notebook_id is the notebook ID, file_path is relative to notebook root
  // For external files: notebook_id is empty, file_path is absolute path
  std::string OpenBuffer(const std::string &notebook_id, const std::string &file_path);

  // Open a virtual buffer for a non-file-backed address, returns buffer ID
  std::string OpenVirtualBuffer(const std::string &address);

  // Close a buffer and free its content
  bool CloseBuffer(const std::string &id);

  // Get buffer by ID, returns nullptr if not found
  Buffer *GetBuffer(const std::string &id);

  // Check whether a buffer is virtual
  bool IsVirtualBuffer(const std::string &id) const;

  // List all open buffers
  std::vector<Buffer *> ListBuffers();

  // Find buffer by path for de-duplication (returns buffer ID or empty string)
  std::string FindBufferByPath(const std::string &notebook_id, const std::string &file_path);

  // Refresh open buffer paths from MetadataStore after path-changing operations
  // (rename, move). For each buffer in the given notebook, queries the store for
  // the current path using the buffer's stable file UUID. Updates buffer path,
  // clears backup cache, discards stale backups, and updates provider path.
  // Skips buffers without a provider or without a file UUID (external files).
  void UpdatePaths(const std::string &notebook_id);

  // Save buffer to disk
  VxCoreError SaveBuffer(const std::string &id);

  // Reload buffer from disk
  VxCoreError ReloadBuffer(const std::string &id);

  // Get buffer content as raw memory (direct access for large files)
  VxCoreError GetBufferContent(const std::string &id, const void **out_data, size_t *out_size);

  // Set buffer content from raw memory
  VxCoreError SetBufferContent(const std::string &id, const void *data, size_t size);

  // Backup file operations
  VxCoreError WriteBackup(const std::string &id);
  VxCoreError HasBackup(const std::string &id, bool &out_has_backup);
  VxCoreError RecoverBackup(const std::string &id);
  VxCoreError DiscardBackup(const std::string &id);
  VxCoreError GetBackupPath(const std::string &id, std::string &out_path);

  // Close all buffers associated with a notebook
  void CloseBuffersForNotebook(const std::string &notebook_id);

  // Get provider for a buffer (returns nullptr if unsupported or not found)
  IBufferProvider *GetProvider(const std::string &buffer_id);

  // Mark that shutdown has been called (prevents destructor from saving)
  void SetShutdownCalled(bool called) { shutdown_called_ = called; }

  // Update buffer records in session config (in-memory only, no disk write)
  void UpdateSessionBuffers();

  // Save buffers to session config and write to disk
  void SaveBuffers();

 private:
  void LoadBuffers();

  ConfigManager *config_manager_ = nullptr;
  NotebookManager *notebook_manager_ = nullptr;
  std::map<std::string, std::unique_ptr<Buffer>> buffers_;
  bool shutdown_called_ = false;
};

}  // namespace vxcore

#endif  // VXCORE_BUFFER_MANAGER_H
