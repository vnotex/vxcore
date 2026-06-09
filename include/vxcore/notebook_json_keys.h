#ifndef VXCORE_NOTEBOOK_JSON_KEYS_H
#define VXCORE_NOTEBOOK_JSON_KEYS_H

// =============================================================================
// PUBLIC vxcore header: SSOT for shared-schema JSON keys.
// =============================================================================
//
// Centralized JSON key constants for notebook-related JSON that vxcore writes
// AND Qt-side code (or other embedders) reads — or vice versa. Both sides MUST
// include THIS header and reference the same `kJsonKey*` constants. Inline
// string literals on either side are forbidden by the `test_json_key_drift`
// gate in the parent project.
//
// Trigger for promoting this header to public:
//   commit 13680f8d ("fix(controllers): use canonical rootFolder JSON key")
//   — Qt-side code read `"root_path"` while vxcore wrote `"rootFolder"`,
//   silently breaking the duplicate-open guard. No exception, no log, no
//   visible error: pure logic drift masked by safe defaults. This header
//   makes that class of bug structurally impossible.
//
// -----------------------------------------------------------------------------
// CONTRACT (READ BEFORE EDITING)
// -----------------------------------------------------------------------------
//
// 1. BACK-COMPAT FOREVER. This is a public vxcore header. The CONSTANTS may
//    grow over time (additive only) but their STRING VALUES are on-disk
//    JSON keys for `session.json`, `vx_notebook/config.json`, hook event
//    payloads, and C-API JSON. NEVER rename an existing constant's string;
//    NEVER delete a constant. To deprecate, mark with a doc-comment and
//    schedule removal in a major-version bump with a migration plan.
//
// 2. SAME STRING, MULTIPLE SCHEMAS — BY DESIGN.
//    Some constants (notably `kJsonKeyId`, `kJsonKeyName`, `kJsonKeyType`,
//    `kJsonKeyMetadata`) represent strings that appear in MULTIPLE schemas:
//      - `"id"` is the notebook id (NotebookRecord), buffer id (Buffer info),
//        workspace id (Workspace JSON), tag id, etc.
//      - `"name"` is notebook name, tag name, file name, folder name, etc.
//      - `"type"` is notebook type ("bundled"/"raw"), node type ("file"/
//        "folder"), event type, etc.
//      - `"metadata"` is notebook metadata, tag metadata, file metadata.
//    ONE constant per string is correct: the constant represents the literal
//    `"id"`, not the SEMANTIC notebook-id. Per-call-site judgment determines
//    which schema is being read/written. Do NOT split into schema-specific
//    variants (`kJsonKeyNotebookRecordId` + `kJsonKeyBufferId`) — that is
//    over-engineering and obscures the SSOT property.
//
// 3. TESTS INTENTIONALLY USE STRING LITERALS.
//    Tests under `libs/vxcore/tests/` and the parent `tests/` directory
//    intentionally still use raw `"rootFolder"`, `"id"`, etc. so they detect
//    drift between THIS header and the actual on-disk format. The
//    `test_json_key_drift` gate in the parent project EXCLUDES test
//    directories. Do not "fix" tests to use the constants.
//
// 4. SYNC KEYS ARE SEPARATE.
//    Sync-related JSON keys (`syncEnabled`, `syncBackend`, `syncRemoteUrl`,
//    `syncIntervalSeconds`, `backend`, `remoteUrl`, etc.) live in their own
//    header: `libs/vxcore/src/sync/sync_json_keys.h`. That header is
//    currently internal to vxcore; this notebook header is the first PUBLIC
//    constant header. If sync keys need to be consumed by Qt-side code in
//    the future, promote `sync_json_keys.h` to public the same way this
//    header was promoted.
//
// -----------------------------------------------------------------------------

namespace vxcore {

// ---------- NotebookRecord keys (session.json per-device state) -------------
// Stored in `session.json`. NOT synced across devices.
inline constexpr const char *kJsonKeyId = "id";
inline constexpr const char *kJsonKeyRootFolder = "rootFolder";
inline constexpr const char *kJsonKeyType = "type";
inline constexpr const char *kJsonKeyReadOnly = "readOnly";

// ---------- NotebookConfig keys (vx_notebook/config.json) -------------------
// Stored per-notebook in `vx_notebook/config.json`. Synced across devices.
// `kJsonKeyId` (above) is also the notebook id field in this schema.
inline constexpr const char *kJsonKeyName = "name";
inline constexpr const char *kJsonKeyDescription = "description";
inline constexpr const char *kJsonKeyAssetsFolder = "assetsFolder";
inline constexpr const char *kJsonKeyMetadata = "metadata";
inline constexpr const char *kJsonKeyTags = "tags";
inline constexpr const char *kJsonKeyTagsModifiedUtc = "tagsModifiedUtc";
inline constexpr const char *kJsonKeyIgnored = "ignored";
inline constexpr const char *kJsonKeyParent = "parent";  // tag parent within tags[]

// ---------- Buffer info keys (returned by vxcore_buffer_get) ----------------
// Buffer info JSON returned by the C API and consumed by Qt-side BufferService.
// `kJsonKeyId` (above) is also the buffer id field in this schema.
inline constexpr const char *kJsonKeyNotebookId = "notebookId";

// ---------- HookEvent / SyncClone keys --------------------------------------
// Typed event payloads (NotebookCloneEvent, FileBeforeOpenEvent, etc.) sent
// to Qt-side hook subscribers and plugins. These ARE shared-schema in the
// sense that plugins read what hookmanager writes. `kJsonKeyTargetDir` is
// the canonical name; `kJsonKeyCloneTargetDir` is kept as a backwards-
// compatible alias for the same string (preserves existing call sites in
// `vxcore_notebook_api.cpp` without churn).
inline constexpr const char *kJsonKeyTargetDir = "targetDir";
inline constexpr const char *kJsonKeyCloneTargetDir = "targetDir";  // alias
inline constexpr const char *kJsonKeyCloneOptions = "options";
inline constexpr const char *kJsonKeyIsReadOnly = "isReadOnly";

}  // namespace vxcore

#endif  // VXCORE_NOTEBOOK_JSON_KEYS_H
