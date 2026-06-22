#ifndef VXCORE_SYNC_JSON_KEYS_H
#define VXCORE_SYNC_JSON_KEYS_H

// Centralized JSON key constants for sync. Eliminates string-literal drift
// between writers (sync_types.cpp, core/notebook.cpp, api/vxcore_sync_api.cpp,
// sync/git/git_options.cpp) and readers (same files + Qt-side JSON consumers
// in src/controllers/ and src/core/services/).
//
// WARNING: DO NOT change the string values, they are on-disk JSON keys and
// must remain back-compatible. Only add new constants here; never rename
// existing ones without a migration plan.
//
// Tests intentionally still use string literals so they detect drift between
// these constants and the actual on-disk format.

namespace vxcore {

// NotebookConfig flat sync fields (per src/sync/AGENTS.md JSON Key Mapping).
// Stored in <notebook_root>/vx_notebook/config.json.
inline constexpr const char *kJsonKeySyncEnabled = "syncEnabled";
inline constexpr const char *kJsonKeySyncBackend = "syncBackend";
inline constexpr const char *kJsonKeySyncRemoteUrl = "syncRemoteUrl";

// Boolean auto-sync gate shared by BOTH the persisted NotebookConfig and the
// C-API SyncConfig blob. true (default) = auto-sync on dirty allowed;
// false = auto-sync suppressed (manual sync only).
inline constexpr const char *kJsonKeyAutoSyncEnabled = "autoSyncEnabled";

// SyncConfig fields (sent over the C API as the config_json argument
// to vxcore_sync_enable). Parsed by SyncConfig::FromJson / ToJson.
inline constexpr const char *kJsonKeyBackend = "backend";
inline constexpr const char *kJsonKeyRemoteUrl = "remoteUrl";
inline constexpr const char *kJsonKeyBackendOptions = "backendOptions";
inline constexpr const char *kJsonKeyExcludePaths = "excludePaths";
inline constexpr const char *kJsonKeyAutoCommitMerges = "autoCommitMerges";

// SyncCredentials JSON keys (sent as credentials_json argument
// to vxcore_sync_enable / vxcore_sync_set_credentials).
inline constexpr const char *kJsonKeyPat = "pat";
inline constexpr const char *kJsonKeyAuthorName = "authorName";
inline constexpr const char *kJsonKeyAuthorEmail = "authorEmail";
inline constexpr const char *kJsonKeyExtra = "extra";

// SyncConflictInfo JSON keys (emitted by vxcore_sync_get_conflicts).
inline constexpr const char *kJsonKeyPath = "path";
inline constexpr const char *kJsonKeyLocalModifiedUtc = "localModifiedUtc";
inline constexpr const char *kJsonKeyRemoteModifiedUtc = "remoteModifiedUtc";
inline constexpr const char *kJsonKeyIsBinary = "isBinary";

// GitOptions keys (parsed from SyncConfig::backend_options for the git backend).
inline constexpr const char *kJsonKeySslVerify = "sslVerify";
inline constexpr const char *kJsonKeyConnectTimeoutMs = "connectTimeoutMs";
inline constexpr const char *kJsonKeyProxyUrl = "proxyUrl";

}  // namespace vxcore

#endif  // VXCORE_SYNC_JSON_KEYS_H
