#ifndef VXCORE_BUFFER_PROVIDER_H
#define VXCORE_BUFFER_PROVIDER_H

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "vxcore/vxcore_types.h"

namespace vxcore {

class Notebook;

// Abstract interface for buffer asset/attachment operations.
// Different implementations handle different file types/layouts:
// - StandardBufferProvider: regular notebook files (assets in vx_assets/<file_uuid>/)
// - ExternalBufferProvider: external files (assets in <filename>_assets/)
// - (Future) TextBundleProvider: text bundle folders (assets inside bundle)
//
// Terminology:
// - "Asset": A resource not tracked in the attachment list
// - "Attachment": A resource tracked in the attachment list
// Protected resources live in an authenticated manifest, not plaintext FileRecord metadata.
//
// Lifecycle: created when buffer opens, destroyed when buffer closes.
// Provider is owned by BufferManager, one per buffer.
class IBufferProvider {
 public:
  virtual ~IBufferProvider() = default;

  // Get the type identifier for this provider (for debugging/logging)
  virtual std::string GetType() const = 0;

  // Cached classification only; no filesystem or key lookup on the ordinary path.
  virtual bool IsEncrypted() const noexcept { return false; }
  virtual VxCoreError GetProtectionError() const noexcept { return VXCORE_OK; }

  // ============ Asset Operations (No Attachment Tracking) ============
  // Ordinary assets use the filesystem; protected assets also update the encrypted manifest.

  // Insert binary data as an asset file.
  // Creates assets folder lazily if it doesn't exist.
  // Does NOT add to attachment list (use InsertAttachment for that).
  //
  // name: desired filename (e.g., "image.png"). If name already exists,
  //       a unique name is generated (e.g., "image_1.png").
  // data: binary content to write
  // out_relative_path: receives path relative to file's parent directory for embedding
  //                    (e.g., "vx_assets/<uuid>/image.png" for notebook files,
  //                     or "<filename>_assets/image.png" for external files)
  // Protected buffers return "vxasset:<resourceId>", never a plaintext filesystem path.
  virtual VxCoreError InsertAssetRaw(const std::string &name, const std::vector<uint8_t> &data,
                                     std::string &out_relative_path) = 0;

  // Copy a file from source path to assets folder.
  // Does NOT add to attachment list (use InsertAttachment for that).
  //
  // source_path: absolute path to source file
  // out_relative_path: receives a relative path, or "vxasset:<resourceId>" when protected
  virtual VxCoreError InsertAsset(const std::string &source_path,
                                  std::string &out_relative_path) = 0;

  // Delete an asset file from the assets folder.
  // Does NOT touch attachment metadata.
  // For notebook-backed files: prefers recycle bin, falls back to permanent delete.
  // For external files: always permanently deletes.
  //
  // relative_path: path as returned by InsertAsset/InsertAssetRaw
  // Protected deletion removes the logical entry; immutable ciphertext is retained.
  virtual VxCoreError DeleteAsset(const std::string &relative_path) = 0;

  // Get absolute filesystem path to the assets folder.
  // Creates folder lazily if it doesn't exist.
  virtual VxCoreError GetAssetsFolder(std::string &out_path) = 0;

  // Get absolute filesystem path to a specific asset.
  // relative_path: path as returned by InsertAsset
  // Protected buffers reject this operation; use ReadResource or explicit export.
  virtual VxCoreError GetAssetAbsolutePath(const std::string &relative_path,
                                           std::string &out_abs_path) = 0;

  // ============ Resource Resolution ============
  // Read a local resource without opening/loading the owning note.
  // Protected URLs must be "vxasset:<resourceId>" from the authenticated open manifest.
  // Ordinary paths use the insertion result (notebook-relative or external-file-parent-relative)
  // and must remain canonically inside that root. Read-only notebooks remain readable.
  // Clear out_data on every failure. Callers own the returned bytes and their lifetime.
  virtual VxCoreError ReadResource(const std::string &resource_url,
                                  std::vector<uint8_t> &out_data) = 0;

  // Get the base path for resolving relative resource URLs in the file's content.
  // For standard files: parent directory of the file (e.g., notebook_root/folder/)
  // For external files: parent directory of the file
  // For TextBundle (future): the bundle directory itself
  virtual VxCoreError GetResourceBasePath(std::string &out_path) = 0;

  // ============ Path Identity ============
  // These methods support BufferManager's ID-based path refresh after move/rename.

  // Get the stable file UUID for this buffer's file (empty if not applicable).
  // Used by BufferManager to refresh paths from MetadataStore after move/rename.
  virtual std::string GetFileId() const { return ""; }

  // Update cached file path after move/rename.
  virtual void SetFilePath(const std::string &path) { (void)path; }
  virtual void SetFileState(const std::string &path, const nlohmann::json &metadata) {
    SetFilePath(path);
    (void)metadata;
  }

  // ============ Attachment Operations (Filesystem + Metadata) ============
  // These methods operate on both filesystem and attachment metadata.
  // For ExternalBufferProvider, metadata operations return VXCORE_ERR_UNSUPPORTED.

  // Copy a file to attachments folder and add to attachment list.
  // For StandardBufferProvider: copies file + adds to FileRecord.attachments
  // For ExternalBufferProvider: copies file only (no metadata tracking)
  //
  // source_path: absolute path to source file
  // out_filename: receives just the filename (not full path) for the attachment
  // Protected insertion encrypts immediately and returns "vxasset:<resourceId>" instead.
  virtual VxCoreError InsertAttachment(const std::string &source_path,
                                       std::string &out_filename) = 0;

  // Delete an attachment file and remove from attachment list.
  // filename: just the filename (not full path)
  // Protected buffers require the logical URL, not its display name.
  virtual VxCoreError DeleteAttachment(const std::string &filename) = 0;

  // Rename an attachment file and update attachment list.
  // old_filename: current filename
  // new_filename: new filename. If exists, a unique name is generated.
  // out_new_filename: receives the actual new filename (may differ if collision)
  // Protected old_filename is the logical URL; new_filename/out_new_filename are display
  // names only, and the logical resource ID does not change.
  virtual VxCoreError RenameAttachment(const std::string &old_filename,
                                       const std::string &new_filename,
                                       std::string &out_new_filename) = 0;

  // List all attachments (from metadata, not filesystem scan).
  // For StandardBufferProvider: returns FileRecord.attachments
  // For ExternalBufferProvider: returns filesystem listing (no metadata)
  // out_filenames: receives list of filenames (not full paths)
  // Protected buffers return authenticated display names only; unloaded notes return LOCKED.
  virtual VxCoreError ListAttachments(std::vector<std::string> &out_filenames) = 0;

  // Enumerate immediate regular files absent from the attachment index, without creating folders.
  virtual VxCoreError ListUnindexedAttachments(std::vector<std::string> &out_filenames) {
    out_filenames.clear();
    return VXCORE_ERR_UNSUPPORTED;
  }

  // Get absolute filesystem path to the attachments folder.
  // Creates folder lazily if it doesn't exist.
  // (Same as GetAssetsFolder - attachments and assets share the same folder)
  virtual VxCoreError GetAttachmentsFolder(std::string &out_path) = 0;
};

// Factory function to create appropriate provider for a notebook file.
// Returns nullptr only if notebook is null. Both bundled and raw notebooks get
// a StandardBufferProvider (attachment methods are internally gated to bundled).
// notebook: the notebook containing the file
// file_path: relative path within the notebook
std::unique_ptr<IBufferProvider> CreateBufferProvider(Notebook *notebook,
                                                      const std::string &file_path);

// Factory function to create provider for an external file (no notebook).
// absolute_file_path: absolute filesystem path to the file
std::unique_ptr<IBufferProvider> CreateBufferProviderForExternal(
    const std::string &absolute_file_path);

}  // namespace vxcore

#endif  // VXCORE_BUFFER_PROVIDER_H
