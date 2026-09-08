#ifndef VXCORE_BUFFER_MANAGER_H
#define VXCORE_BUFFER_MANAGER_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "buffer.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class ConfigManager;
class EventManager;
class IBufferProvider;
class NotebookManager;

class BufferManager {
 public:
  BufferManager(ConfigManager *config_manager, NotebookManager *notebook_manager);
  ~BufferManager();

  // Open a buffer, authenticating protected candidates before returning an ID.
  // Plaintext content remains lazy. Session restoration uses LoadBuffers instead.
  // notebook_id is empty and file_path absolute for external files.
  VxCoreError OpenBuffer(const std::string &notebook_id, const std::string &file_path,
                        std::string &out_id);

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

  // Check if buffer's file has been modified or deleted externally.
  // Updates internal buffer state (query with GetState() afterwards).
  VxCoreError CheckExternalChanges(const std::string &id);

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

  // Sparse protected-only registry; lock checks never walk ordinary buffers.
  bool HasProtectedBuffers() const noexcept;

  // Get provider for a buffer (returns nullptr if unsupported or not found)
  IBufferProvider *GetProvider(const std::string &buffer_id);

  // Mark that shutdown has been called (prevents destructor from saving)
  void SetShutdownCalled(bool called) { shutdown_called_ = called; }

  void SetEventManager(EventManager *event_manager) { event_manager_ = event_manager; }

  // Update buffer records in session config (in-memory only, no disk write)
  void UpdateSessionBuffers();

  // Save buffers to session config and write to disk
  void SaveBuffers();

 private:
  void LoadBuffers();
  void EmitEvent(const char *event_name, const nlohmann::json &event_data);
  VxCoreError EnsureProtectedContent(Buffer &buffer);
  void RefreshFileState(Buffer &buffer, const std::string &path, const std::string &metadata);
  void UpdateProtectedRegistration(Buffer &buffer);

  ConfigManager *config_manager_ = nullptr;
  NotebookManager *notebook_manager_ = nullptr;
  EventManager *event_manager_ = nullptr;
  std::map<std::string, std::unique_ptr<Buffer>> buffers_;
  std::unique_ptr<std::unordered_set<Buffer *>> protected_buffers_;
  bool shutdown_called_ = false;
};

}  // namespace vxcore

#endif  // VXCORE_BUFFER_MANAGER_H
