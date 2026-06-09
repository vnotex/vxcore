#ifndef VXCORE_CORE_NOTEBOOK_JSON_KEYS_H
#define VXCORE_CORE_NOTEBOOK_JSON_KEYS_H

// Centralized JSON key constants for notebook configuration and operations.
// Eliminates string-literal drift between writers (BundledNotebook, API layer,
// controllers) and readers (same files + Qt-side JSON consumers).
//
// WARNING: DO NOT change the string values, they are on-disk JSON keys and
// must remain back-compatible. Only add new constants here; never rename
// existing ones without a migration plan.
//
// Tests intentionally still use string literals so they detect drift between
// these constants and the actual on-disk format.

namespace vxcore {

// NotebookRecord JSON keys (stored in session.json for per-device state).
// These fields are NOT synced across devices; they persist locally only.
inline constexpr const char *kJsonKeyReadOnly = "readOnly";

// SyncClone request payload keys (sent to vxcore_sync_clone).
// Defines the configuration for cloning a remote notebook into a local directory.
inline constexpr const char *kJsonKeyCloneTargetDir = "targetDir";
inline constexpr const char *kJsonKeyCloneOptions = "options";

}  // namespace vxcore

#endif  // VXCORE_CORE_NOTEBOOK_JSON_KEYS_H
