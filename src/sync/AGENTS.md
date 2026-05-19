# Agent Guidelines for src/sync

src/sync contains the pluggable notebook synchronization backend layer.
It provides an abstract `ISyncBackend` interface, a `SyncManager` orchestrator, and all supporting types.
GitSyncBackend is implemented; WebDAV/OneDrive/etc. remain future.

## Architecture

```
C API (vxcore_sync_*)
    │
    ▼
SyncManager                         ← Orchestrator (per-notebook backend dispatch)
    │
    ├── backends_  map<id, unique_ptr<ISyncBackend>>   ← Concrete backend instances
    ├── states_    map<id, SyncState>                   ← Per-notebook sync state
    ├── configs_   map<id, SyncConfig>                  ← Per-notebook sync config (runtime)
    └── dirty_notebooks_  set<id>                       ← Notebooks with unsynced changes (via EventManager)

ISyncBackend                        ← Pure virtual interface (8 methods)
    │
    ├── GitSyncBackend (libgit2-based)
    ├── WebDavSyncBackend (future)
    ├── OneDriveSyncBackend (future)
    ├── DropboxSyncBackend (future)
    └── BaiduNetDiskSyncBackend (future)
```

### Data Flow

```
User Code
    │
    ▼
vxcore_sync_enable(ctx, notebook_id, config_json)
    │
    ▼
VxCoreContext::sync_manager (unique_ptr<SyncManager>)
    │
    ├── ValidateNotebook(notebook_id)
    │     ├── NotebookManager::GetNotebook()
    │     ├── Reject if nullptr → VXCORE_ERR_NOT_FOUND
    │     └── Reject if Raw → VXCORE_ERR_UNSUPPORTED
    │
    └── EnableSync / TriggerSync / GetStatus / ...
          ├── Check states_ map → SYNC_NOT_ENABLED if absent
          └── Check backends_ map → NOT_IMPLEMENTED if no backend registered
```

### Validation Flow (Two-Step)

Every SyncManager method that operates on a notebook follows this pattern:

1. **`ValidateNotebook(notebook_id)`** — Checks notebook exists and is bundled (not raw)
2. **State check** — Verifies sync is enabled for this notebook (`states_` map)
3. **Backend check** — Verifies a concrete backend is registered (`backends_` map)

| Condition | Error Code |
|-----------|------------|
| Notebook not found | `VXCORE_ERR_NOT_FOUND` |
| Notebook is raw type | `VXCORE_ERR_UNSUPPORTED` |
| Sync not enabled | `VXCORE_ERR_SYNC_NOT_ENABLED` |
| No backend registered | `VXCORE_ERR_NOT_IMPLEMENTED` |

## File Reference

| File | Purpose |
|------|---------|
| `sync_types.h` | Enums (`SyncState`, `SyncFileStatus`, `SyncConflictResolution`), structs (`SyncConfig`, `SyncProgress`, `SyncFileInfo`, `SyncConflictInfo`, `SyncCredentials`), `SyncProgressCallback` typedef, `SyncConfig::FromJson()`/`ToJson()` |
| `sync_backend.h` | `ISyncBackend` pure virtual class (9 methods including `SetCredentials`) |
| `sync_manager.h` | `SyncManager` class: per-notebook dispatch, dirty tracking via `EventManager` |
| `sync_manager.cpp` | `SyncManager` implementation (validation, state management, event subscription, dirty tracking) |
| `git/git_sync_backend.h` | `GitSyncBackend` class declaration (inherits `ISyncBackend`) — composition root |
| `git/git_sync_backend.cpp` | Composition root implementation: orchestrates Sync/Push/Pull, retry loop, delegates phases |
| `git/git_handles.h` | `unique_ptr` aliases for libgit2 handle types (used in new extracted code only) |
| `git/git_error_translator.{h,cpp}` | `TranslateGitError(int rc) -> VxCoreError` — libgit2 error class mapping |
| `git/git_defaults.{h,cpp}` | `kDefaultGitignore`, `kDefaultGitattributes`, `WriteIfMissing`, `BuildGitignoreContent` |
| `git/git_config_fixer.{h,cpp}` | `RebindCoreWorktree` — pre-open INI rewriter for stale `core.worktree` paths |
| `git/gitkeep_sweeper.{h,cpp}` | `EnsureGitkeepFiles` + skip-list helpers + `IsOwnedMarker` (data-safety zero-byte rule) |
| `git/git_credential_callback.{h,cpp}` | `GitCredentialPayload` struct + `GitSyncBackendCredentialCb` + `MakeRemoteCallbacks` (payload-based dispatch; no friend) |
| `git/git_repo_bootstrap.{h,cpp}` | `Initialize` state machine: re-open / clone-into-empty / init-and-push helpers |
| `git/git_sync_pipeline.{h,cpp}` | `GitSyncPipeline` class: Stage/Commit/Fetch/Rebase/Push + ApplyDefaultGitConfig + RemoteHasRefs + ContinueRebaseAfterResolution |
| `git/git_conflict_resolver.{h,cpp}` | `GitConflictResolver` class: GetConflicts + ResolveConflict |
| `git/libgit2_init.h` | `LibGit2Init` RAII helper that calls `git_libgit2_init`/`shutdown` |
| `git/libgit2_init.cpp` | Reference-counted `git_libgit2_init` wrapper (thread-safe) |

### Related Files Outside src/sync

| File | What's There |
|------|-------------|
| `include/vxcore/vxcore_types.h` | Error codes 21-25 (`SYNC_IN_PROGRESS`, `CONFLICT`, `AUTH_FAILED`, `NETWORK`, `NOT_ENABLED`), `VxCoreEventCallback` typedef |
| `include/vxcore/vxcore.h` | 8 C API declarations (`vxcore_sync_{enable,disable,trigger,get_status,get_conflicts,resolve_conflict,set_credentials,enable_with_credentials}`) |
| `src/api/vxcore_sync_api.cpp` | C API implementations with JSON serialization and null checks |
| `src/core/context.h` | `VxCoreContext` owns `sync_manager`, `event_manager`, `work_queue_manager` (member order matters — see Thread Safety) |
| `src/core/event_manager.h/.cpp` | `EventManager` subscribe/emit system — SyncManager subscribes to file/folder events |
| `src/core/event_names.h` | Event name constants: `file.created`, `file.saved`, `file.deleted`, `file.moved`, `folder.created`, `folder.deleted`, `notebook.opened`, `notebook.closed` |
| `src/core/notebook.h` | `NotebookConfig` sync fields: `sync_enabled`, `sync_backend`, `sync_remote_url`, `sync_interval_seconds` |
| `src/core/notebook.cpp` | `FromJson()`/`ToJson()` for sync fields |
| `tests/test_sync.cpp` | 14 test cases: error paths, raw rejection, config roundtrip, `backend_options`/`extra` roundtrip |
| `tests/test_event_manager.cpp` | 18 tests including 4 SyncManager dirty-tracking integration tests |

## Types Reference

### Enums

```cpp
enum class SyncState {
  kIdle,          // No sync activity
  kStaging,       // Preparing local changes (e.g., git add)
  kFetching,      // Downloading remote changes
  kAnalyzing,     // Comparing local vs remote
  kMerging,       // Resolving differences
  kPushing,       // Uploading local changes
  kError,         // Sync failed
  kConflicted     // Unresolved conflicts exist
};

enum class SyncFileStatus {
  kUnchanged, kModifiedLocal, kModifiedRemote, kConflicted,
  kAddedLocal, kAddedRemote, kDeletedLocal, kDeletedRemote
};

enum class SyncConflictResolution {
  kKeepBoth,      // Rename conflict copy as file.sync-conflict-{timestamp}.ext
  kKeepLocal,     // Overwrite remote with local
  kKeepRemote     // Overwrite local with remote
};
```

### Structs

| Struct | Fields | Notes |
|--------|--------|-------|
| `SyncConfig` | `enabled`, `backend`, `remote_url`, `interval_seconds`, `exclude_paths`, `backend_options` | Has `FromJson()`/`ToJson()`. Default interval: 300s. Default excludes: `*.vswp`, `vx_notebook/vx_sync/`. `backend_options` is an opaque JSON bag for backend-specific config. |
| `SyncProgress` | `message`, `percentage`, `current_state` | Passed to `SyncProgressCallback` during sync operations |
| `SyncFileInfo` | `path`, `status` | Per-file sync status |
| `SyncConflictInfo` | `path`, `local_modified_utc`, `remote_modified_utc`, `is_binary` | Conflict metadata for resolution UI |
| `SyncCredentials` | `username`, `password`, `ssh_public_key_path`, `ssh_private_key_path`, `personal_access_token`, `author_name`, `author_email`, `extra` | NOT stored in notebook config (lives in session config, per-device). `extra` is an opaque JSON bag for backend-specific credentials. |

### Callback

```cpp
using SyncProgressCallback = std::function<void(const SyncProgress &progress, void *userdata)>;
```

## ISyncBackend Interface

```cpp
class ISyncBackend {
public:
  virtual ~ISyncBackend() = default;

  virtual VxCoreError Initialize(const std::string &root_folder,
                                 const SyncConfig &config) = 0;
  virtual VxCoreError Shutdown() = 0;
  virtual VxCoreError Sync(SyncProgressCallback callback, void *userdata) = 0;
  virtual VxCoreError Push(SyncProgressCallback callback, void *userdata) = 0;
  virtual VxCoreError Pull(SyncProgressCallback callback, void *userdata) = 0;
  virtual VxCoreError GetStatus(std::vector<SyncFileInfo> &out_files) = 0;
  virtual VxCoreError GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) = 0;
  virtual VxCoreError ResolveConflict(const std::string &path,
                                      SyncConflictResolution resolution) = 0;
};
```

| Method | When Called | Expected Behavior |
|--------|-----------|-------------------|
| `Initialize` | On `EnableSync` after backend is created | Set up working directory, connect to remote, validate config |
| `Shutdown` | On `DisableSync` or context destruction | Close connections, flush state, release resources |
| `Sync` | On `TriggerSync` (full round-trip) | Stage + fetch + merge + push. Report progress via callback |
| `Push` | Manual push only | Upload local changes without pulling |
| `Pull` | Manual pull only | Download remote changes without pushing |
| `GetStatus` | On `GetSyncStatus` | Return per-file status list |
| `GetConflicts` | On `GetConflicts` | Return list of unresolved conflicts |
| `ResolveConflict` | On `ResolveConflict` | Apply resolution strategy to a specific file |

## C API Reference

All functions take `VxCoreContextHandle` as first parameter. All return `VxCoreError`.
Output strings must be freed with `vxcore_string_free()`.

| Function | Parameters | Notes |
|----------|-----------|-------|
| `vxcore_sync_enable` | `notebook_id`, `config_json` | JSON keys: `backend`, `remoteUrl`, `intervalSeconds`, `backendOptions` |
| `vxcore_sync_disable` | `notebook_id` | Clears backend, state, and config for notebook |
| `vxcore_sync_trigger` | `notebook_id` | Returns `SYNC_NOT_ENABLED` or `NOT_IMPLEMENTED` until backend exists. Clears dirty state on success. |
| `vxcore_sync_get_status` | `notebook_id`, `out_status_json` | Output: `{"state":"idle","files":[{"path":"...","status":"..."}]}` |
| `vxcore_sync_get_conflicts` | `notebook_id`, `out_conflicts_json` | Output: `{"conflicts":[{"path":"...","localModifiedUtc":0,"remoteModifiedUtc":0,"isBinary":false}]}` |
| `vxcore_sync_resolve_conflict` | `notebook_id`, `path`, `resolution` | Resolution strings: `"keep_both"`, `"keep_local"`, `"keep_remote"` |
| `vxcore_sync_set_credentials` | `notebook_id`, `credentials_json` | Rotate credentials on already-enabled notebook. JSON keys: `pat`, `authorName`, `authorEmail`, `extra` |
| `vxcore_sync_enable_with_credentials` | `notebook_id`, `config_json`, `credentials_json` | Enable + set credentials atomically (needed when Initialize requires auth, e.g., authenticated git clone) |

### JSON Key Mapping (C API ↔ C++ Struct)

| JSON Key (camelCase) | C++ Member (snake_case) | Type |
|---------------------|------------------------|------|
| `backend` | `SyncConfig::backend` | string |
| `remoteUrl` | `SyncConfig::remote_url` | string |
| `intervalSeconds` | `SyncConfig::interval_seconds` | int |
| `backendOptions` | `SyncConfig::backend_options` | object (opaque) |
| `excludePaths` | `SyncConfig::exclude_paths` | string array |
| `extra` | `SyncCredentials::extra` | object (opaque) |
| `syncEnabled` | `NotebookConfig::sync_enabled` | bool |
| `syncBackend` | `NotebookConfig::sync_backend` | string |
| `syncRemoteUrl` | `NotebookConfig::sync_remote_url` | string |
| `syncIntervalSeconds` | `NotebookConfig::sync_interval_seconds` | int |
| `localModifiedUtc` | `SyncConflictInfo::local_modified_utc` | int64 |
| `remoteModifiedUtc` | `SyncConflictInfo::remote_modified_utc` | int64 |
| `isBinary` | `SyncConflictInfo::is_binary` | bool |

## NotebookConfig Sync Fields

Sync configuration is stored in `vx_notebook/config.json` as flat fields on `NotebookConfig`:

```cpp
struct NotebookConfig {
  // ... existing fields ...
  bool sync_enabled = false;
  std::string sync_backend;          // e.g., "git", "webdav"
  std::string sync_remote_url;       // e.g., "https://github.com/user/repo.git"
  int sync_interval_seconds = 300;   // Auto-sync interval (default 5 minutes)
};
```

**Design decisions:**
- Flat fields (not a nested `SyncConfig` struct) — keeps `NotebookConfig` simple
- Backend type + remote URL are portable (stored in notebook folder)
- Credentials are NOT stored here (see `SyncCredentials` — lives in session config, per-device)
- `metadata.db` is NOT in the notebook folder (lives in `local_data_folder`)

## Design Constraints

| Constraint | Rationale |
|-----------|-----------|
| Bundled notebooks only | Raw notebooks have no `vx_notebook/` folder for sync state |
| No Qt dependencies | vxcore is a pure C/C++ library |
| No network code in skeleton | Backends implement their own transport |
| No credential storage | Credentials are per-device, stored in session config by the Qt layer |
| Recycle bin IS synced | `vx_recycle_bin/` is included so deletions propagate across devices |
| Sync state directory: `vx_notebook/vx_sync/` | Backend-specific state (e.g., `.git/`) lives here |
| Conflict naming: `file.sync-conflict-{timestamp}.ext` | Syncthing-style, keeps both versions |

## Git Sync Backend

`GitSyncBackend` is the first concrete `ISyncBackend` implementation. It uses
[libgit2](https://libgit2.org/) (vendored as a nested submodule under
`libs/vxcore/third_party/libgit2`). The implementation is split across multiple
focused files under `libs/vxcore/src/sync/git/` (see File Map below).

> **Note**: Earlier revisions of this doc claimed `GitSyncBackend` was built
> "whenever `VXCORE_ENABLE_GIT_SYNC` is `ON` (default)". That option does NOT
> exist in `libs/vxcore/CMakeLists.txt` — git sync is always built. The option
> is tracked as a deferred audit doc-drift fix.

### File Map

| File | Purpose |
|------|---------|
| `git/git_sync_backend.h` | `GitSyncBackend` class declaration (inherits `ISyncBackend`) — composition root |
| `git/git_sync_backend.cpp` | Composition root: Sync/Push/Pull orchestration + retry loop; delegates to extracted classes/helpers |
| `git/git_handles.h` | RAII `unique_ptr` aliases for 14 libgit2 handle types (used in NEW extracted code only — existing manual cleanup blocks unchanged) |
| `git/git_error_translator.{h,cpp}` | `TranslateGitError(int rc)` — collapses libgit2 error classes into `VxCoreError` |
| `git/git_defaults.{h,cpp}` | Default `.gitignore`/`.gitattributes` content + `WriteIfMissing` + `BuildGitignoreContent` |
| `git/git_config_fixer.{h,cpp}` | `RebindCoreWorktree` — pre-open INI rewriter that fixes stale `core.worktree` paths before libgit2 opens the repo |
| `git/gitkeep_sweeper.{h,cpp}` | `EnsureGitkeepFiles` + 5 helpers (skip-list, IsOwnedMarker, IsEffectivelyEmpty); see Gitkeep Injection section below |
| `git/git_credential_callback.{h,cpp}` | `GitCredentialPayload` (PAT-only struct), `GitSyncBackendCredentialCb`, `MakeRemoteCallbacks` (centralized wiring). Friend-free; payload-based dispatch |
| `git/git_repo_bootstrap.{h,cpp}` | `Initialize` state machine: `OpenExistingRepo` / `BootstrapFromEmptyRemote` (clone) / `BootstrapToEmptyRemote` (init+push) / `ScrubGitlink` / `InitEmptyRepoWithRemote` |
| `git/git_sync_pipeline.{h,cpp}` | `GitSyncPipeline` class — single-shot phase methods: `StageAll`, `CommitIndex`, `FetchOrigin`, `RebaseOntoOrigin`, `PushOrigin`, `ApplyDefaultGitConfig`, `RemoteHasRefs`, `ContinueRebaseAfterResolution`. Retry loop stays in `GitSyncBackend::Sync` (composition root). |
| `git/git_conflict_resolver.{h,cpp}` | `GitConflictResolver` class — `GetConflicts` + `ResolveConflict` |
| `git/libgit2_init.{h,cpp}` | `LibGit2Init` RAII helper (reference-counted `git_libgit2_init` wrapper) |

### Repository Layout

The git working tree IS the notebook root, but the gitdir is hidden inside the
`vx_notebook/vx_sync/` metadata folder so it never appears in the user's
notebook listing. This split is created via `git_repository_init_ext` with
`GIT_REPOSITORY_INIT_NO_DOTGIT_DIR` and explicit `workdir_path`/`gitdir_path`
arguments:

```
<notebook_root>/                  ← workdir (notebook content)
├── My Note.md
├── images/
└── vx_notebook/
    └── vx_sync/                  ← gitdir (refs, objects, index, HEAD, config)
        ├── HEAD
        ├── config
        ├── objects/
        └── refs/
```

`SyncConfig::exclude_paths` defaults to `*.vswp` and `vx_notebook/vx_sync/`,
which keeps editor swap files and the gitdir itself out of every commit.

### Authentication

| Method | How |
|--------|-----|
| HTTPS PAT | `SyncCredentials::personal_access_token` is sent as the password with username `x-access-token` (GitHub/GitLab/Gitea convention) |
| Anonymous | If no PAT is set, libgit2 falls back to no credentials (works for public-read remotes) |
| SSH | NOT SUPPORTED in v1 — libssh2 is intentionally not linked |

### Commit Author

Defaults to `"VNote Sync" <sync@vnote.local>`. Callers may override per-sync via
`SyncCredentials::author_name` and `SyncCredentials::author_email`. The commit
author is NOT persisted in `NotebookConfig` (kept session-local).

### Sync Semantics

`Sync()` performs a single round-trip:

1. **Stage** — `git add -A` on the workdir, honoring `exclude_paths`.
2. **Commit** — Auto-commit with timestamped message if the index has changes.
3. **Fetch** — `git fetch origin` for the tracked branch.
4. **Rebase** — Replay local commits onto `origin/<branch>`.
5. **Push** — `git push origin <branch>` (fast-forward only).

Non-fast-forward push errors trigger up to **3 retries** with exponential
backoff (`100ms`, `500ms`, `2000ms`), each retry refetching and rebasing.
`Sync()` is **synchronous** in v1 — it blocks the calling thread until the
round-trip finishes or a non-recoverable error is returned.

### Conflict Workflow

If the rebase hits a conflict, libgit2 leaves the rebase **in progress** on
disk and `Sync()` returns `VXCORE_ERR_SYNC_CONFLICT`. The caller then:

1. `GetConflicts()` — reads conflict entries from the libgit2 index and
   returns one `SyncConflictInfo` per conflicted path.
2. `ResolveConflict(path, kKeepBoth)` — writes the local version to
   `<file>.sync-conflict-<unix_ts>.<ext>` (Syncthing-style sidecar) and stages
   the remote version as the resolved file.
   `kKeepLocal` and `kKeepRemote` overwrite without a sidecar.
3. When all conflicts are resolved, `GitSyncBackend` automatically calls
   `git_rebase_continue` until the rebase finishes, then resumes the push
   step. No explicit "commit resolution" call is needed.

### Limitations / Deferred to v2

The following are explicitly **out of scope for v1** and tracked separately:

- SSH transport (libssh2 not linked, no key-based auth)
- Username/password basic auth (only PAT is supported over HTTPS)
- OAuth flows (device code, PKCE, refresh tokens)
- QtKeychain integration (PAT lives in session config in plaintext for now)
- Public C API for `Push` / `Pull` (only `Sync` is wired through `vxcore_sync_*`)
- Branch switching / non-default branches per push
- Commit log / history inspection API
- Multiple remotes per notebook
- Git-LFS support
- Persisted custom commit author (override is per-call only)

See `docs/sync-evolution-plan.md` for implementation history and remaining ideas.

## Gitkeep Injection (Git Sync Backend)

`GitSyncBackend::EnsureGitkeepFiles()` is a pre-stage sweep called from `Sync()` and `Push()` immediately before `StageAll()`. It writes `.gitkeep` marker files into empty user folders so git preserves the folder structure on push + clone, and removes those markers when a folder later gains real content. Folders are otherwise dropped silently by git, so without this sweep an empty VNote folder vanishes from the remote.

### Lifecycle

- **Just-in-time at sync.** No event subscriptions, no `QFileSystemWatcher`. The sweep walks the notebook tree once per `Sync()`/`Push()` call, while `op_mutex_` is held by the caller.
- **No-op on idempotent state.** Re-running sync without any tree change makes zero filesystem mutations.
- **Always-on for sync-enabled notebooks.** No per-notebook setting, no user-facing toggle.
- **Exceptions never escape.** All filesystem operations use `std::error_code` overloads; the iterator loop is wrapped in `try/catch`. Marker write/remove failures are logged via `VXCORE_LOG_WARN` and the sweep continues — the next sync retries.

### Marker ownership (CRITICAL for data safety)

| Marker file size | Treated as | Auto-cleanup eligible? |
|---|---|---|
| Exactly 0 bytes | VNote-owned (we wrote it) | YES — removed when folder becomes non-empty |
| Any non-zero size | User-authored | **NEVER** removed, regardless of folder contents |

This protects users who manually create a `.gitkeep` with custom content (e.g., a comment explaining why the folder must exist). The size check lives in `IsOwnedMarker()` inside `git/gitkeep_sweeper.cpp` (anonymous-namespace helper). Do NOT relax this rule without a deliberate plan-level decision.

### Skip-list (folders that NEVER receive `.gitkeep`)

| Pattern | Scope | Reason |
|---|---|---|
| `vx_notebook/...` | **Root-relative** (first path component only) | vxcore metadata; the `vx.json` files inside `vx_notebook/contents/` keep that subtree non-empty already |
| `vx_recycle_bin/...` | **Root-relative** (first path component only) | Recycle-bin policy |
| Notebook root itself (`""` or `"."`) | Always | Adding `.gitkeep` at root would trip the both-non-empty guard in `GitSyncBackend::Initialize()` against fresh remotes |
| Any component starting with `.` | Every depth | Hidden directories — not VNote-managed |
| Any component starting with `_v_` | Every depth | Reserved prefix |

**Root-relative scope** means user folders nested with reserved names (e.g., `MyNotes/vx_notebook/`) ARE eligible for `.gitkeep` — those are legitimate user data. Only the actual top-level `vx_notebook/` and `vx_recycle_bin/` are skipped.

### Effective emptiness

A folder counts as "effectively empty" (and thus eligible for a `.gitkeep` seed) when its single-level `directory_iterator` yields zero entries that satisfy either:

- **(a)** Regular file NOT matching `SyncConfig::exclude_paths` globs AND NOT named `.gitkeep`, OR
- **(b)** Sub-directory whose POSIX relative path is NOT in the skip-list above.

Consequences:
- Folder containing only `*.vswp` (matches default `exclude_paths`) → effectively empty → gets `.gitkeep`.
- Folder containing only `.cache/` (dot-prefix, skipped) → effectively empty → gets `.gitkeep`.
- Folder `A` containing only `A/B/` where `B` itself has a `.gitkeep` → A is NOT effectively empty (B counts as content) → A does NOT get its own marker.

Each folder is its own decision — the recursive walker naturally creates markers at the deepest empty point and the parent inherits "non-empty" status via the marker-bearing child.

### Implementation pointers

- File-local helpers in the anonymous namespace of `git/gitkeep_sweeper.cpp`: `kGitkeepFilename`, `GitkeepSkipPrefixes()`, `ShouldSkipForGitkeep()`, `IsOwnedMarker()`, `IsEffectivelyEmpty()`.
- Public free function: `void EnsureGitkeepFiles(const std::string &root_folder, const SyncConfig &config);` in `git/gitkeep_sweeper.h` — uses the two-pass walker pattern documented in [libs/vxcore/AGENTS.md → Common Patterns → Two-pass directory walker](../../AGENTS.md#two-pass-directory-walker-recursive_directory_iterator).
- Call sites: `Sync()` and `Push()` in `git/git_sync_backend.cpp`, each contains exactly one `EnsureGitkeepFiles(root_folder_, config_);` line immediately before `pipeline.StageAll(...)`. Both methods hold `op_mutex_` at the insertion point, so no extra synchronization is needed.
- The existing staging callback (the one that excludes `vx_notebook/vx_sync/`) does NOT filter `.gitkeep` — verified during T5 of the gitkeep plan; no exemption clause needed.

### Tests

| Test target | Cases | Covers |
|---|---|---|
| `tests/test_gitkeep_basic.cpp` | 10 | Empty folder seeding, nested empty, skip-list (vx_notebook, vx_recycle_bin, dot, `_v_`, root), idempotence, nested reserved names |
| `tests/test_gitkeep_cleanup.cpp` | 7 | Auto-remove on real-file-added, user-authored preservation, owned-preserved-when-still-empty, excluded-only effective emptiness, skipped-subdir effective emptiness, gitkeep-bearing-subdir parent, zero-byte-only cleanup distinction |
| `tests/test_gitkeep_roundtrip.cpp` | 1 | Full push → bare-remote → clone-elsewhere; empty folder survives with `.gitkeep` in clone, no `.gitkeep` at clone root, no `.gitkeep` under `vx_notebook/` or `vx_recycle_bin/` |

All three test targets follow the [standalone test pattern](../../AGENTS.md#standalone-test-pattern-tests-touching-libgit2--gitsyncbackend) documented in `libs/vxcore/AGENTS.md` (direct-compile the git sync sources from `git/`, link `git_sync_test_helpers`, local `vxcore_set_test_mode` shim, `clone_bare_to_workdir` instead of `git_clone()`). The list of sources to direct-compile is centralized in `libs/vxcore/cmake/git_sync_sources.cmake` (`VXCORE_GIT_SYNC_SOURCES_ABS` variant) — both `src/CMakeLists.txt` and `tests/CMakeLists.txt` `include()` this file.

## How to Add a New Sync Backend

### Step 1: Create the Backend Class

Create `src/sync/my_sync_backend.h` and `src/sync/my_sync_backend.cpp`:

```cpp
// src/sync/my_sync_backend.h
#ifndef VXCORE_MY_SYNC_BACKEND_H
#define VXCORE_MY_SYNC_BACKEND_H

#include "sync_backend.h"

namespace vxcore {

class MySyncBackend : public ISyncBackend {
 public:
  MySyncBackend();
  ~MySyncBackend() override;

  VxCoreError Initialize(const std::string &root_folder,
                         const SyncConfig &config) override;
  VxCoreError Shutdown() override;
  VxCoreError Sync(SyncProgressCallback callback, void *userdata) override;
  VxCoreError Push(SyncProgressCallback callback, void *userdata) override;
  VxCoreError Pull(SyncProgressCallback callback, void *userdata) override;
  VxCoreError GetStatus(std::vector<SyncFileInfo> &out_files) override;
  VxCoreError GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) override;
  VxCoreError ResolveConflict(const std::string &path,
                              SyncConflictResolution resolution) override;

 private:
  std::string root_folder_;
  SyncConfig config_;
  bool initialized_ = false;
};

}  // namespace vxcore

#endif  // VXCORE_MY_SYNC_BACKEND_H
```

### Step 2: Implement the Methods

```cpp
// src/sync/my_sync_backend.cpp
#include "my_sync_backend.h"
#include "utils/logger.h"

namespace vxcore {

MySyncBackend::MySyncBackend() = default;
MySyncBackend::~MySyncBackend() { Shutdown(); }

VxCoreError MySyncBackend::Initialize(const std::string &root_folder,
                                      const SyncConfig &config) {
  root_folder_ = root_folder;
  config_ = config;

  // TODO: Set up sync working directory at root_folder_/vx_notebook/vx_sync/
  // TODO: Connect to remote using config.remote_url
  // TODO: Validate remote is reachable

  initialized_ = true;
  VXCORE_LOG_INFO("MySyncBackend initialized for: %s", root_folder.c_str());
  return VXCORE_OK;
}

VxCoreError MySyncBackend::Shutdown() {
  if (!initialized_) return VXCORE_OK;
  // TODO: Close connections, flush state
  initialized_ = false;
  return VXCORE_OK;
}

VxCoreError MySyncBackend::Sync(SyncProgressCallback callback, void *userdata) {
  if (!initialized_) return VXCORE_ERR_UNKNOWN;

  // Report progress via callback
  if (callback) {
    SyncProgress progress;
    progress.current_state = SyncState::kStaging;
    progress.message = "Staging local changes...";
    progress.percentage = 0.1f;
    callback(progress, userdata);
  }

  // TODO: Implement actual sync logic
  // 1. Stage local changes (kStaging)
  // 2. Fetch remote changes (kFetching)
  // 3. Analyze differences (kAnalyzing)
  // 4. Merge changes (kMerging)
  // 5. Push to remote (kPushing)

  return VXCORE_OK;
}

// ... implement Push, Pull, GetStatus, GetConflicts, ResolveConflict similarly
}  // namespace vxcore
```

### Step 3: Register in SyncManager

Add a factory branch to `SyncManager::EnableSyncImpl()` in `sync_manager.cpp`:

```cpp
// In SyncManager::EnableSyncImpl(), after the existing "git" branch:
if (config.backend == "git") {
  backend = std::make_unique<GitSyncBackend>();
} else if (config.backend == "my_backend") {
  backend = std::make_unique<MySyncBackend>();
}
```

The existing `EnableSyncImpl` handles credential forwarding, rollback on failure,
and state management — you only need to add the factory branch.

### Step 4: Add to Build

Add the new `.cpp` file to `src/CMakeLists.txt`:

```cmake
set(VXCORE_SOURCES
  # ... existing sources ...
  sync/my_sync_backend.cpp
)
```

### Step 5: Add Tests

Add test cases to `tests/test_sync.cpp` or create a new test file. Follow the existing test pattern:

```cpp
int test_my_sync_backend_init() {
  std::cout << "  Running test_my_sync_backend_init..." << std::endl;
  cleanup_test_dir(get_test_path("test_my_sync"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_my_sync").c_str(),
                               "{\"name\":\"My Sync Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Enable with your backend
  err = vxcore_sync_enable(ctx, notebook_id,
                           "{\"backend\":\"my_backend\",\"remoteUrl\":\"test://repo\"}");
  ASSERT_EQ(err, VXCORE_OK);

  // Trigger should now succeed (backend exists)
  err = vxcore_sync_trigger(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_my_sync"));
  std::cout << "  ✓ test_my_sync_backend_init passed" << std::endl;
  return 0;
}
```

### Checklist for New Backend

- [ ] Create `src/sync/{name}_sync_backend.h` with class inheriting `ISyncBackend`
- [ ] Create `src/sync/{name}_sync_backend.cpp` implementing all 9 methods (including `SetCredentials`)
- [ ] Add factory branch in `SyncManager::EnableSyncImpl()`
- [ ] Add `.cpp` to `src/CMakeLists.txt` `VXCORE_SOURCES`
- [ ] Add test cases to `tests/test_sync.cpp`
- [ ] If backend needs external library (e.g., libgit2), add `find_package` + `target_link_libraries` in `CMakeLists.txt`
- [ ] Use `SyncProgressCallback` to report progress during `Sync()`, `Push()`, `Pull()`
- [ ] Handle credentials via `SyncCredentials` struct (passed at runtime, not stored in config)
- [ ] Read backend-specific options from `SyncConfig::backend_options` and `SyncCredentials::extra`
- [ ] Store backend-specific state in `<notebook_root>/vx_notebook/vx_sync/`
- [ ] Respect `SyncConfig::exclude_paths` when scanning for changes
- [ ] Return appropriate error codes (see table in Validation Flow section)

## Existing Tests

Run sync tests:
```bash
ctest --test-dir build -C Debug -R test_sync
ctest --test-dir build -C Debug -R test_event_manager   # includes dirty-tracking tests
```

### test_sync.cpp (14 tests)

| Test | What It Verifies |
|------|-----------------|
| `test_sync_error_codes` | Error code values 21-25 and their error messages exist |
| `test_sync_enable_disable` | Enable/disable roundtrip on bundled notebook returns `VXCORE_OK` |
| `test_sync_status_not_enabled` | `GetStatus` before `EnableSync` returns `SYNC_NOT_ENABLED` |
| `test_sync_trigger_not_implemented` | `TriggerSync` with no backend returns `NOT_IMPLEMENTED` |
| `test_sync_raw_notebook_rejected` | `EnableSync` on raw notebook returns `UNSUPPORTED` |
| `test_sync_nonexistent_notebook` | `EnableSync` on missing notebook returns `NOT_FOUND` |
| `test_notebook_config_sync_fields` | `NotebookConfig` sync fields roundtrip through JSON |
| `test_sync_config_from_json_with_backend_options` | `SyncConfig::FromJson` parses `backendOptions` |
| `test_sync_config_to_json_includes_backend_options` | `SyncConfig::ToJson` includes `backendOptions` when non-empty |
| `test_sync_config_roundtrip_backend_options` | `FromJson`→`ToJson` roundtrip preserves nested `backendOptions` |
| `test_sync_config_from_json_without_backend_options` | `FromJson` without `backendOptions` leaves it null (backward compat) |
| `test_sync_credentials_extra_field` | `SyncCredentials::extra` holds opaque JSON |
| `test_sync_enable_with_backend_options_via_c_api` | C API passes `backendOptions` through to `SyncConfig` |
| `test_sync_config_to_json_omits_empty_backend_options` | `ToJson` omits `backendOptions` when empty |

### test_event_manager.cpp (dirty-tracking subset, 4 tests)

| Test | What It Verifies |
|------|-----------------|
| `test_sync_dirty_on_file_created` | File creation marks sync-enabled notebook as dirty |
| `test_sync_dirty_cleared_after_trigger` | `ClearDirty` removes notebook from dirty set |
| `test_sync_dirty_not_marked_for_non_sync_notebook` | File ops on non-sync notebook do NOT mark dirty |
| `test_sync_dirty_multiple_ops_single_entry` | Multiple file/folder ops produce only one dirty entry |

## Event-Driven Dirty Tracking

`SyncManager::SetEventManager()` subscribes to `file.created`, `file.saved`,
`file.deleted`, `file.moved`, `folder.created`, and `folder.deleted` events.
When an event fires for a notebook that has sync enabled (`configs_` map),
the notebook ID is added to `dirty_notebooks_` (a `std::set`, so duplicates
are ignored).

**Caller responsibility:** The caller polls `GetDirtyNotebooks()` periodically
(e.g., on a timer) and calls `TriggerSync()` for each dirty notebook.
`TriggerSync()` automatically calls `ClearDirty()` on success. This keeps
debounce/interval policy in the caller, not in vxcore.

```
file.created event → SyncManager marks notebook dirty
                   → caller's timer fires
                   → GetDirtyNotebooks() returns ["nb-123"]
                   → vxcore_sync_trigger(ctx, "nb-123")
                   → on success, dirty state cleared
```

## Thread Safety

`SyncManager` methods (`EnableSync`, `TriggerSync`, etc.) are NOT thread-safe.
The caller must serialize access (e.g., via signals/slots or a mutex).

`SyncManager` dirty tracking (`GetDirtyNotebooks`/`ClearDirty`) IS thread-safe
(protected by its own mutex), since events may fire from any thread.

**`VxCoreContext` member ordering constraint:** In `context.h`, `EventManager`
and `WorkQueueManager` are declared before `SyncManager` so they are destroyed
after it. `SyncManager::~SyncManager()` unsubscribes from `EventManager` during
destruction — if `EventManager` were destroyed first, this would be a
use-after-free.

`Sync()` is **synchronous** in v1 — it blocks the calling thread. The caller
can use `WorkQueue` to run sync on a worker thread:

```cpp
// Qt caller example:
auto *sync_queue = ctx->work_queue_manager->GetOrCreate("sync");

// On timer tick (main thread):
for (const auto &nb_id : ctx->sync_manager->GetDirtyNotebooks()) {
  sync_queue->Enqueue([ctx, nb_id] {
    ctx->sync_manager->TriggerSync(nb_id);  // blocks on worker thread, not UI
  });
}

// Worker thread (QThread):
while (!stopped) {
  vxcore_work_queue_process_next(ctx_handle, "sync", 500);
}
```
