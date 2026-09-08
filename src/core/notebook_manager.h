#ifndef VXCORE_NOTEBOOK_MANAGER_H
#define VXCORE_NOTEBOOK_MANAGER_H

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "notebook.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class ConfigManager;
class EventManager;

class NotebookManager {
 public:
  NotebookManager(ConfigManager *config_manager);
  ~NotebookManager();

  void SetEventManager(EventManager *event_manager);

  VxCoreError CreateNotebook(const std::string &root_folder, NotebookType type,
                             const std::string &config_json, std::string &out_notebook_id);

  VxCoreError OpenNotebook(const std::string &root_folder, std::string &out_notebook_id);

  VxCoreError CloseNotebook(const std::string &notebook_id);

  VxCoreError GetNotebookConfig(const std::string &notebook_id, std::string &out_config_json);

  VxCoreError UpdateNotebookConfig(const std::string &notebook_id, const std::string &config_json);

  VxCoreError ListNotebooks(std::string &out_notebooks_json);

  Notebook *GetNotebook(const std::string &notebook_id);

  // Resolve an absolute path to its containing notebook.
  // Returns the notebook ID and relative path within that notebook.
  // Returns VXCORE_ERR_NOT_FOUND if path is not within any open notebook.
  VxCoreError ResolvePathToNotebook(const std::string &absolute_path, std::string &out_notebook_id,
                                    std::string &out_relative_path);

  // Resolve a node UUID to its containing notebook and relative path.
  // Searches all open notebooks' metadata stores for the given node ID.
  // Returns VXCORE_ERR_NOT_FOUND if no open notebook contains a node with this ID.
  VxCoreError ResolveNodeById(const std::string &node_id, std::string &out_notebook_id,
                              std::string &out_relative_path);

  // T14 of open-notebook-remote-readonly: persist the per-device read-only
  // flag for a notebook into session.json. Updates the matching
  // NotebookRecord.read_only and rewrites session config. No-op if the
  // notebook is unknown. Best-effort -- I/O failures are logged but never
  // propagated (matches the Notebook::SetLastSyncUtc semantics; the
  // runtime flag has already been flipped by SetReadOnly(), so the worst
  // outcome of a write failure is that the next session reverts to the
  // pre-T14 default).
  void RecordNotebookReadOnly(const std::string &notebook_id, bool read_only);

  // Encryption-only entry points. Lazy state remains null for ordinary use.
  // Existing notebook lifecycle/metadata synchronization remains caller-owned.
  struct EncryptionSetup;
  VxCoreError PrepareNotebookEncryption(const std::string &notebook_id,
                                        const std::string &source_notebook_id,
                                        const void *password, size_t password_size,
                                        EncryptionSetup *&out_setup);
  VxCoreError CommitNotebookEncryption(EncryptionSetup *setup);
  void FreeEncryptionSetup(EncryptionSetup *setup);
  VxCoreError UnlockNotebookEncryption(const std::string &notebook_id,
                                       const void *password, size_t password_size);
  // Protected candidates only: reuse an authenticated session MK to unlock this
  // independent NK, with no password/KDF. LOCKED if the vault has no cached MK;
  // AUTH_FAILED on a different password envelope or failed NK authentication.
  VxCoreError UnlockNotebookWithCachedMaster(const std::string &notebook_id);
  VxCoreError GetEncryptionStatus(const std::string &notebook_id, const char *file_path,
                                  std::string &out_status_json);
  VxCoreError LockAllEncryption();

  // Private storage consumers retain acquired NK/MK shared_ptr leases for every
  // protected buffer/job/read, including while using a derived DK. An NK lease
  // is also its unlock-generation identity; never cache only a raw Key pointer.
  // Registry mutexes cover only lease/map changes, never KDF, IO or callbacks.
  VxCoreError InstallMasterKey(const NotebookEncryption::KeyEnvelope &authenticated_envelope,
                               NotebookEncryption::Key &&master_key);
  VxCoreError AcquireMasterKey(const std::string &vault_id,
                              std::shared_ptr<const NotebookEncryption::Key> &out_key) const;
  VxCoreError LockMasterKeys();

 private:
  struct EncryptionSession;
  std::shared_ptr<EncryptionSession> GetEncryptionSession(bool create) const;
  VxCoreError RegisterEncryptionOwner(Notebook &notebook, EncryptionSession &session);
  VxCoreError InstallEncryptionKeys(Notebook &notebook, EncryptionSession &session,
                                    const NotebookEncryption::KeyEnvelope &envelope,
                                    std::shared_ptr<const NotebookEncryption::Key> master_key,
                                    std::shared_ptr<const NotebookEncryption::Key> notebook_key,
                                    const std::string *key_file_bytes);
  void LoadOpenNotebooks();
  Notebook *FindNotebookByRootFolder(const std::string &root_folder);

  NotebookRecord *FindNotebookRecord(const std::string &id);
  VxCoreError UpdateNotebookRecord(const Notebook &notebook);

  nlohmann::json ToNotebookConfig(const Notebook &notebook) const;

  void DeleteNotebookLocalData(const Notebook &notebook);

  ConfigManager *config_manager_ = nullptr;
  EventManager *event_manager_ = nullptr;
  // Atomic shared_ptr access is used only by encryption entry points. The sparse
  // session owns setup tombstones and protected owners, not ordinary notebooks.
  mutable std::shared_ptr<EncryptionSession> encryption_session_;
  std::map<std::string, std::unique_ptr<Notebook>> notebooks_;
};

}  // namespace vxcore

#endif
