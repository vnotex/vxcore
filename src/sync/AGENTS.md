# Agent Guidelines for src/sync

src/sync contains the pluggable notebook synchronization backend layer.
It provides an abstract `ISyncBackend` interface, a `SyncManager` orchestrator, and all supporting types.
No concrete backend (Git, WebDAV, etc.) is implemented yet — this is the interface skeleton only.

## Architecture

```
C API (vxcore_sync_*)
    │
    ▼
SyncManager                         ← Orchestrator (per-notebook backend dispatch)
    │
    ├── backends_  map<id, unique_ptr<ISyncBackend>>   ← Concrete backend instances
    ├── states_    map<id, SyncState>                   ← Per-notebook sync state
    └── configs_   map<id, SyncConfig>                  ← Per-notebook sync config (runtime)

ISyncBackend                        ← Pure virtual interface (8 methods)
    │
    ├── GitSyncBackend (future)     ← libgit2-based
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
| `sync_types.h` | Enums (`SyncState`, `SyncFileStatus`, `SyncConflictResolution`), structs (`SyncConfig`, `SyncProgress`, `SyncFileInfo`, `SyncConflictInfo`, `SyncCredentials`), `SyncProgressCallback` typedef |
| `sync_backend.h` | `ISyncBackend` pure virtual class (8 methods) |
| `sync_manager.h` | `SyncManager` class declaration (7 public + 1 private method, 3 maps) |
| `sync_manager.cpp` | `SyncManager` skeleton implementation (validation + state management, no actual sync) |

### Related Files Outside src/sync

| File | What's There |
|------|-------------|
| `include/vxcore/vxcore_types.h` | Error codes 21-25 (`SYNC_IN_PROGRESS`, `CONFLICT`, `AUTH_FAILED`, `NETWORK`, `NOT_ENABLED`) |
| `include/vxcore/vxcore.h` | 6 C API declarations (`vxcore_sync_{enable,disable,trigger,get_status,get_conflicts,resolve_conflict}`) |
| `src/api/vxcore_sync_api.cpp` | C API implementations with JSON serialization and null checks |
| `src/core/context.h` | `VxCoreContext::sync_manager` (`unique_ptr<SyncManager>`) |
| `src/core/notebook.h` | `NotebookConfig` sync fields: `sync_enabled`, `sync_backend`, `sync_remote_url`, `sync_interval_seconds` |
| `src/core/notebook.cpp` | `FromJson()`/`ToJson()` for sync fields |
| `tests/test_sync.cpp` | 7 test cases covering error paths, raw rejection, config roundtrip |

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
| `SyncConfig` | `enabled`, `backend`, `remote_url`, `interval_seconds`, `exclude_paths` | Has `FromJson()`/`ToJson()`. Default interval: 300s. Default excludes: `*.vswp`, `vx_notebook/vx_sync/` |
| `SyncProgress` | `message`, `percentage`, `current_state` | Passed to `SyncProgressCallback` during sync operations |
| `SyncFileInfo` | `path`, `status` | Per-file sync status |
| `SyncConflictInfo` | `path`, `local_modified_utc`, `remote_modified_utc`, `is_binary` | Conflict metadata for resolution UI |
| `SyncCredentials` | `username`, `password`, `ssh_public_key_path`, `ssh_private_key_path` | NOT stored in notebook config (lives in session config, per-device) |

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
| `vxcore_sync_enable` | `notebook_id`, `config_json` | JSON keys: `backend`, `remoteUrl`, `intervalSeconds` |
| `vxcore_sync_disable` | `notebook_id` | Clears backend, state, and config for notebook |
| `vxcore_sync_trigger` | `notebook_id` | Returns `SYNC_NOT_ENABLED` or `NOT_IMPLEMENTED` until backend exists |
| `vxcore_sync_get_status` | `notebook_id`, `out_status_json` | Output: `{"state":"idle","files":[{"path":"...","status":"..."}]}` |
| `vxcore_sync_get_conflicts` | `notebook_id`, `out_conflicts_json` | Output: `{"conflicts":[{"path":"...","localModifiedUtc":0,"remoteModifiedUtc":0,"isBinary":false}]}` |
| `vxcore_sync_resolve_conflict` | `notebook_id`, `path`, `resolution` | Resolution strings: `"keep_both"`, `"keep_local"`, `"keep_remote"` |

### JSON Key Mapping (C API ↔ C++ Struct)

| JSON Key (camelCase) | C++ Member (snake_case) | Type |
|---------------------|------------------------|------|
| `backend` | `SyncConfig::backend` | string |
| `remoteUrl` | `SyncConfig::remote_url` | string |
| `intervalSeconds` | `SyncConfig::interval_seconds` | int |
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

Add backend creation logic to `SyncManager::EnableSync()` in `sync_manager.cpp`:

```cpp
VxCoreError SyncManager::EnableSync(const std::string &notebook_id,
                                    const SyncConfig &config) {
  VxCoreError err = ValidateNotebook(notebook_id);
  if (err != VXCORE_OK) return err;

  configs_[notebook_id] = config;
  states_[notebook_id] = SyncState::kIdle;

  // Create backend based on config.backend string
  std::unique_ptr<ISyncBackend> backend;
  if (config.backend == "my_backend") {
    backend = std::make_unique<MySyncBackend>();
  }
  // else if (config.backend == "git") { ... }
  // else if (config.backend == "webdav") { ... }

  if (backend) {
    auto *notebook = notebook_manager_->GetNotebook(notebook_id);
    err = backend->Initialize(notebook->GetRootFolder(), config);
    if (err != VXCORE_OK) return err;
    backends_[notebook_id] = std::move(backend);
  }

  return VXCORE_OK;
}
```

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
- [ ] Create `src/sync/{name}_sync_backend.cpp` implementing all 8 methods
- [ ] Add `#include` and factory branch in `SyncManager::EnableSync()`
- [ ] Add `.cpp` to `src/CMakeLists.txt` `VXCORE_SOURCES`
- [ ] Add test cases to `tests/test_sync.cpp`
- [ ] If backend needs external library (e.g., libgit2), add `find_package` + `target_link_libraries` in `CMakeLists.txt`
- [ ] Use `SyncProgressCallback` to report progress during `Sync()`, `Push()`, `Pull()`
- [ ] Handle credentials via `SyncCredentials` struct (passed at runtime, not stored in config)
- [ ] Store backend-specific state in `<notebook_root>/vx_notebook/vx_sync/`
- [ ] Respect `SyncConfig::exclude_paths` when scanning for changes
- [ ] Return appropriate error codes (see table in Validation Flow section)

## Existing Tests

Run sync tests:
```bash
ctest --test-dir build -C Debug -R test_sync
```

| Test | What It Verifies |
|------|-----------------|
| `test_sync_error_codes` | Error code values 21-25 and their error messages exist |
| `test_sync_enable_disable` | Enable/disable roundtrip on bundled notebook returns `VXCORE_OK` |
| `test_sync_status_not_enabled` | `GetStatus` before `EnableSync` returns `SYNC_NOT_ENABLED` |
| `test_sync_trigger_not_implemented` | `TriggerSync` with no backend returns `NOT_IMPLEMENTED` |
| `test_sync_raw_notebook_rejected` | `EnableSync` on raw notebook returns `UNSUPPORTED` |
| `test_sync_nonexistent_notebook` | `EnableSync` on missing notebook returns `NOT_FOUND` |
| `test_notebook_config_sync_fields` | `NotebookConfig` sync fields roundtrip through JSON (create → read → update → read) |

## Thread Safety

`SyncManager` is NOT thread-safe. If sync operations are triggered from a background timer, the caller (Qt layer) must serialize access via signals/slots or a mutex.

For backends that need async operations (network I/O), the recommended pattern is:
1. `SyncManager::TriggerSync()` is called on the main thread
2. The backend internally spawns a worker thread for I/O
3. Progress is reported via `SyncProgressCallback` (which the caller marshals to the UI thread)
4. The backend returns `VXCORE_ERR_SYNC_IN_PROGRESS` if a sync is already running
