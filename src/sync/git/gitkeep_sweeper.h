#ifndef VXCORE_SYNC_GIT_GITKEEP_SWEEPER_H
#define VXCORE_SYNC_GIT_GITKEEP_SWEEPER_H

#include <string>

#include "sync/sync_types.h"  // For SyncConfig

namespace vxcore {

// Pre-stage sweep: write .gitkeep marker files into empty user folders so
// git preserves the folder structure on push + clone, and remove those
// markers when a folder later gains real content.
//
// Data-safety guarantees (DO NOT relax without a deliberate plan-level decision):
//   - .gitkeep files of EXACTLY 0 bytes are treated as VNote-owned and may
//     be auto-removed when the folder becomes non-empty.
//   - .gitkeep files of any NON-ZERO size are treated as user-authored and
//     are NEVER auto-removed, regardless of folder contents. This protects
//     users who manually create a .gitkeep with custom content.
//
// Skip-list (folders that NEVER receive .gitkeep):
//   - vx_notebook/...           (root-relative first component only)
//   - vx_recycle_bin/...        (root-relative first component only)
//   - notebook root itself ("" or ".")
//   - any path component starting with "." (hidden)
//   - any path component starting with "_v_" (reserved prefix)
//
// Lifecycle:
//   - Just-in-time at sync. No event subscriptions; no QFileSystemWatcher.
//   - Caller MUST hold op_mutex_ (or equivalent serialization).
//   - No-op on idempotent state. Re-running with no tree change is zero writes.
//   - Exceptions never escape. All FS operations use std::error_code overloads.
//     Marker write/remove failures are logged via VXCORE_LOG_WARN and the
//     sweep continues — the next sync retries.
void EnsureGitkeepFiles(const std::string &root_folder, const SyncConfig &config);

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GITKEEP_SWEEPER_H
