#ifndef VXCORE_SYNC_ENCRYPTION_GUARD_H
#define VXCORE_SYNC_ENCRYPTION_GUARD_H

#include <string>
#include <vector>

#include "vxcore/vxcore_types.h"

namespace vxcore {
class Notebook;

// Offline inspection remains authoritative even after runtime sync unregistration.
VxCoreError CheckNotebookEncryptionSyncState(const std::string &metadata_folder);

// Read-only retirement gate, including unregistered Git index conflicts and DAV journals.
VxCoreError CheckNotebookSyncReconfiguration(const std::string &metadata_folder);

// Called under the caller's IO gate before a deferred backend can replace an envelope.
// A successful lock is intentionally retained after publication (including partial failure).
VxCoreError PrepareNotebookEncryptionSyncApply(Notebook &notebook,
                                               const std::vector<std::string> &protected_paths);

}  // namespace vxcore

#endif  // VXCORE_SYNC_ENCRYPTION_GUARD_H
