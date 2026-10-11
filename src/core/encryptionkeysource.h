#ifndef VXCORE_ENCRYPTIONKEYSOURCE_H
#define VXCORE_ENCRYPTIONKEYSOURCE_H

#include <filesystem>
#include <memory>

#include "encryptednote.h"

namespace vxcore {

// Captures unauthenticated, bounded key metadata for a specific note or backup.
// Callers authenticate keys and revalidate this source after any unlocked work.
class EncryptionKeySource {
 public:
  virtual ~EncryptionKeySource() = default;
  virtual const EncryptedNote::KeyEnvelope &Envelope() const noexcept = 0;
  // Null only for an explicitly selected legacy vault (empty note_path).
  virtual const EncryptedNote::ObjectHeader *Header() const noexcept = 0;
  virtual bool IsLegacy() const noexcept = 0;
  virtual VxCoreError Revalidate() const = 0;

  // metadata_folder locates legacy storage and the containing notebook root.
  // Embedded notes never inspect that folder or its legacy vault/conflict state.
  static VxCoreError Read(const std::filesystem::path &note_path,
                          const std::filesystem::path &metadata_folder,
                          std::unique_ptr<EncryptionKeySource> &out_source);
};

}  // namespace vxcore

#endif  // VXCORE_ENCRYPTIONKEYSOURCE_H
