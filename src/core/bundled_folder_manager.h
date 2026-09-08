#ifndef VXCORE_BUNDLED_FOLDER_MANAGER_H
#define VXCORE_BUNDLED_FOLDER_MANAGER_H

#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "folder.h"
#include "folder_manager.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class Notebook;
class NodeTransfer;

class BundledFolderManager : public FolderManager {
 public:
  explicit BundledFolderManager(Notebook *notebook);
  ~BundledFolderManager() override;

  VxCoreError InitOnCreation() override;

  VxCoreError GetFolderConfig(const std::string &folder_path,
                              std::string &out_config_json) override;

  VxCoreError CreateFolder(const std::string &parent_path, const std::string &folder_name,
                           std::string &out_folder_id) override;

  VxCoreError DeleteFolder(const std::string &folder_path) override;

  VxCoreError UpdateFolderMetadata(const std::string &folder_path,
                                   const std::string &metadata_json) override;

  VxCoreError UpdateNodeTimestamps(const std::string &node_path, int64_t created_utc,
                                   int64_t modified_utc) override;

  VxCoreError GetFolderMetadata(const std::string &folder_path,
                                std::string &out_metadata_json) override;

  VxCoreError RenameFolder(const std::string &folder_path, const std::string &new_name) override;

  VxCoreError MoveFolder(const std::string &src_path, const std::string &dest_parent_path) override;

  VxCoreError CopyFolder(const std::string &src_path, const std::string &dest_parent_path,
                         const std::string &new_name, std::string &out_folder_id) override;

  VxCoreError CreateFile(const std::string &folder_path, const std::string &file_name,
                         std::string &out_file_id) override;

  VxCoreError DeleteFile(const std::string &file_path) override;

  VxCoreError UpdateFileMetadata(const std::string &file_path,
                                 const std::string &metadata_json) override;

  VxCoreError UpdateFileTags(const std::string &file_path, const std::string &tags_json) override;

  VxCoreError TagFile(const std::string &file_path, const std::string &tag_name) override;

  VxCoreError UntagFile(const std::string &file_path, const std::string &tag_name) override;

  VxCoreError GetFileAttachments(const std::string &file_path,
                                 std::string &out_attachments_json) override;

  VxCoreError UpdateFileAttachments(const std::string &file_path,
                                    const std::string &attachments_json) override;

  VxCoreError AddFileAttachment(const std::string &file_path,
                                const std::string &attachment) override;

  VxCoreError DeleteFileAttachment(const std::string &file_path,
                                   const std::string &attachment) override;

  VxCoreError GetFileInfo(const std::string &file_path, std::string &out_file_info_json) override;

  VxCoreError GetFileInfo(const std::string &file_path, const FileRecord **out_record) override;

  VxCoreError GetFileMetadata(const std::string &file_path,
                              std::string &out_metadata_json) override;

  VxCoreError RenameFile(const std::string &file_path, const std::string &new_name) override;

  VxCoreError MoveFile(const std::string &src_file_path,
                       const std::string &dest_folder_path) override;

  VxCoreError CopyFile(const std::string &src_file_path, const std::string &dest_folder_path,
                       const std::string &new_name, std::string &out_file_id) override;

  VxCoreError ImportFile(const std::string &folder_path, const std::string &external_file_path,
                         std::string &out_file_id) override;

  VxCoreError ImportFolder(const std::string &dest_folder_path,
                           const std::string &external_folder_path,
                           const std::string &suffix_allowlist,
                           std::string &out_folder_id) override;

  void IterateAllFiles(
      std::function<bool(const std::string &, const FileRecord &)> callback) override;

  VxCoreError FindFilesByTag(const std::string &tag_name, std::string &out_files_json) override;

  VxCoreError ListFolderContents(const std::string &folder_path, bool include_folders_info,
                                 FolderContents &out_contents) override;

  VxCoreError SetChildrenOrder(const std::string &folder_path,
                               const std::string &ordered_json) override;

  void ClearCache() override;

  VxCoreError IndexNode(const std::string &node_path) override;

  VxCoreError UnindexNode(const std::string &node_path) override;

  VxCoreError ListExternalNodes(const std::string &folder_path,
                                FolderContents &out_contents) override;

  // Syncs the MetadataStore from config files (vx.json)
  // Called on notebook open to rebuild cache from ground truth
  // Returns VXCORE_OK on success
  VxCoreError SyncMetadataStoreFromConfigs();

  // Collects EVERY node id (folder AND file, root folder included) by walking
  // the on-disk vx.json tree. NEVER consults the MetadataStore: bundled
  // notebooks populate it lazily, so the store cannot prove an id's absence.
  // This is the authoritative id oracle used to reject import collisions.
  VxCoreError CollectAllNodeIds(std::vector<std::string> &out_ids);

  // Attaches a STAGED imported folder bundle to @dest_folder_path under @name.
  // See vxcore_folder_attach_imported() in the public header for the full
  // contract; this implements the journaled commit protocol.
  VxCoreError AttachImportedFolder(const std::string &dest_folder_path, const std::string &name,
                                   const std::string &staging_dir, std::string &out_folder_id);

  // Replays or rolls back incomplete import journals left by a crash.
  // Called on notebook open BEFORE SyncMetadataStoreFromConfigs().
  VxCoreError RecoverImports(int *out_recovered_count);

  // Recovers private cross-notebook transfer publication/removal journals.
  VxCoreError RecoverTransfers(int *out_recovered_count);

  // The caller quiesces views/buffers and holds maintenance then notebook IO.
  // Old plaintext buffers remain alive until the durable replacement is published.
  VxCoreError ProtectNote(const std::string &file_path, const void *body, size_t body_size,
                          const std::string &resource_plan_json, std::string &out_path);
  VxCoreError CreateEncryptedNote(const std::string &parent_path, const std::string &name,
                                  const std::string &editor_type, const void *body,
                                  size_t body_size, std::string &out_id);
  // Invoked only when existing transfer discovery encounters its encryption child.
  VxCoreError RecoverEncryptionTransactions(const std::filesystem::path &directory,
                                           int *out_recovered_count);

  // Returns the path to the recycle bin folder
  std::string GetRecycleBinPath() const;

  // Bridges the private FileContentExistsOnDisk / FolderContentExistsOnDisk
  // helpers to the FolderManager base interface so the C-API layer (which only
  // holds a FolderManager*) can gate read/access operations on bundled nodes
  // whose content has disappeared from disk.
  bool NodeContentExistsOnDisk(const std::string &relative_path, bool is_folder) const override;

  // Internal transaction surface for NodeTransfer. These are deliberately not
  // added to FolderManager because raw cross-notebook transfer is unsupported.
  VxCoreError TransferGetFolderConfig(const std::string &folder_path, FolderConfig **out_config) {
    return GetFolderConfig(folder_path, out_config);
  }
  VxCoreError TransferSaveFolderConfigAtomic(const std::string &folder_path,
                                             const FolderConfig &config) {
    return SaveFolderConfigAtomic(folder_path, config);
  }
  std::string TransferGetConfigPath(const std::string &folder_path) const {
    return GetConfigPath(folder_path);
  }
  std::string TransferGetContentPath(const std::string &folder_path) const {
    return GetContentPath(folder_path);
  }
  void TransferInvalidateCache(const std::string &folder_path) { InvalidateCache(folder_path); }

  VxCoreError ContainsEncryptedNotes(const std::string &folder_path, bool &out_contains);
 private:
  VxCoreError RecycleProtectedNode(const std::string &path, bool folder);
  VxCoreError MoveProtectedFile(const std::string &path, const std::string &destination_folder);
  VxCoreError CommitProtectedRelocation(nlohmann::json &journal);
  VxCoreError CompleteProtectedRelocation(const std::filesystem::path &directory,
                                          nlohmann::json &journal);
  VxCoreError NormalizeRecycledAssets(nlohmann::json &journal, const std::string &folder_path,
                                      const std::filesystem::path &destination);
  VxCoreError CommitEncryptedNote(const std::string &source_path, const std::string &parent_path,
                                  const std::string &name, const std::string &editor_type,
                                  const void *body, size_t body_size,
                                  const nlohmann::json &plan, std::string &out_result);
  VxCoreError CompleteEncryptionTransaction(const std::filesystem::path &directory,
                                            nlohmann::json &journal);
  VxCoreError GetFolderConfig(const std::string &folder_path, FolderConfig **out_config,
                              const std::string *parent_id = nullptr);
  VxCoreError LoadFolderConfig(const std::string &folder_path,
                               std::unique_ptr<FolderConfig> &out_config);
  VxCoreError SaveFolderConfig(const std::string &folder_path, const FolderConfig &config);

  // Crash-safe variant of SaveFolderConfig: writes <vx.json>.tmp, flushes it to
  // stable storage, then renames it over the live file. Used by the import
  // commit point, where a truncated-then-interrupted in-place rewrite (what
  // SaveFolderConfig does) would destroy the destination parent's index.
  VxCoreError SaveFolderConfigAtomic(const std::string &folder_path, const FolderConfig &config,
                                     bool emit_event = true);

  std::string GetConfigPath(const std::string &folder_path) const;
  std::string GetContentPath(const std::string &folder_path) const;

  void CacheConfig(const std::string &folder_path, std::unique_ptr<FolderConfig> config);
  FolderConfig *GetCachedConfig(const std::string &folder_path);
  void InvalidateCache(const std::string &folder_path);

  FileRecord *FindFileRecord(FolderConfig &config, const std::string &file_name);

  // Sync a single folder's data to MetadataStore after loading from vx.json
  // This is the lazy sync implementation - called when a folder is accessed
  void SyncFolderToStore(const std::string &folder_path, const FolderConfig &config,
                         const std::string &parent_folder_id);

  // Get the parent folder's ID (UUID) for a given folder path
  std::string GetParentFolderId(const std::string &folder_path);

  // Recursively process a copied folder tree: regenerate UUIDs, rename assets, rewrite content
  VxCoreError ProcessCopiedFolderTree(const std::string &dest_path, const std::string &parent_id,
                                      const std::string &new_name = "");

  // Check if a file's content exists on disk at the expected location.
  // Returns true IFF the on-disk content path is a regular file.
  bool FileContentExistsOnDisk(const std::string &relative_path) const;

  // Check if a folder's content exists on disk at the expected location.
  // Returns true IFF the on-disk content path is a directory.
  bool FolderContentExistsOnDisk(const std::string &relative_path) const;

  // Recycle bin helpers
  std::string GenerateUniqueRecycleBinName(const std::string &name) const;
  std::string GenerateUniqueFileName(const std::string &folder_abs_path,
                                     const std::string &desired_name) const;
  VxCoreError MoveToRecycleBin(const std::filesystem::path &source_path);
  std::map<std::string, std::unique_ptr<FolderConfig>> config_cache_;
};

}  // namespace vxcore

#endif
