# Sync Evolution Plan — Event System, Async Work Queue, Backend Genericity

> Tracks three foundational improvements to vxcore's sync layer.
> Prerequisite ordering: Phase 1 → Phase 2 → Phase 3 (each builds on the previous).

## Phase 1: Generic Work Queue (`src/core/work_queue.h`)

**Why first:** Both the event system (Phase 2) and async sync (Phase 2b) need a
thread-safe way to enqueue work from any thread and drain it from a caller-owned
worker thread. Rather than building two ad-hoc queues, we introduce one reusable
primitive that all modules can share.

**Design:**

```
WorkQueue                              ← mutex-protected std::deque<WorkItem>
  Enqueue(WorkItem)                    ← any thread; O(1), takes lock briefly
  ProcessNext(timeout) → bool          ← worker thread; blocks until item or timeout
  ProcessAll()                         ← drain everything currently queued
  Size() → size_t                      ← atomic read
  Shutdown()                           ← unblocks any waiting ProcessNext
```

- `WorkItem` is `std::function<void()>` — maximum flexibility, zero coupling.
- Uses `std::mutex` + `std::condition_variable` (C++17 stdlib only, no platform deps).
- vxcore does NOT create threads — the caller (Qt/Swift/Kotlin) owns the worker
  thread and calls `ProcessNext()` in a loop.

**C API surface:**

```c
// Create/destroy a shared work queue (owned by context)
// ProcessNext blocks up to timeout_ms; returns 1 if work was executed, 0 if timeout.
VXCORE_API int vxcore_work_queue_process_next(VxCoreContextHandle ctx, int timeout_ms);
VXCORE_API int vxcore_work_queue_process_all(VxCoreContextHandle ctx);
VXCORE_API int vxcore_work_queue_size(VxCoreContextHandle ctx);
VXCORE_API void vxcore_work_queue_shutdown(VxCoreContextHandle ctx);
```

**Caller usage pattern (Qt example):**

```cpp
// In a QThread::run() or QtConcurrent worker:
while (!stopped) {
  vxcore_work_queue_process_next(ctx, 500);  // blocks up to 500ms
}
```

**Files to create/modify:**

| Action | File |
|--------|------|
| Create | `src/core/work_queue.h` — `WorkQueue` class |
| Create | `src/core/work_queue.cpp` — implementation |
| Create | `tests/test_work_queue.cpp` — enqueue/drain, shutdown, timeout, multi-producer |
| Modify | `src/core/context.h` — add `std::unique_ptr<WorkQueue> work_queue` |
| Create | `src/api/vxcore_work_queue_api.cpp` — C API wrappers |
| Modify | `include/vxcore/vxcore.h` — declare C API functions |
| Modify | `src/CMakeLists.txt` — add source files |
| Modify | `tests/CMakeLists.txt` — add test |

**Tasks:**

- [x] P1-1: Implement `WorkQueue` + `WorkQueueManager` classes (mutex + condvar + deque)
- [x] P1-2: Add C API (`vxcore_work_queue_*` with named queues)
- [x] P1-3: Wire into `VxCoreContext` (created/destroyed with context)
- [x] P1-4: Tests — 16 tests (11 WorkQueue + 5 WorkQueueManager + 1 C API)
- [x] P1-5: Update AGENTS.md with work queue docs

**Test plan (`tests/test_work_queue.cpp`):**

- [ ] T1-1: Enqueue single item, ProcessNext returns it
- [ ] T1-2: Enqueue multiple items, ProcessAll drains all in FIFO order
- [ ] T1-3: ProcessNext with timeout returns false when queue is empty
- [ ] T1-4: Size() reflects enqueue/dequeue accurately
- [ ] T1-5: Shutdown() unblocks a waiting ProcessNext
- [ ] T1-6: Enqueue after Shutdown is rejected (or no-op)
- [ ] T1-7: Multi-producer: N threads enqueue concurrently, single consumer drains all
- [ ] T1-8: Producer-consumer: producer and consumer threads run simultaneously
- [ ] T1-9: Stress test: 10000 items enqueued and drained correctly
- [ ] T1-10: C API roundtrip: `vxcore_work_queue_process_next` via context handle
- [ ] T1-11: Double shutdown is safe (idempotent)

---

## Phase 2: Event System (`src/core/event_manager.h`)

**Why:** vxcore currently has no way to notify listeners when files are
created/saved/deleted. Sync must be triggered explicitly. An event system lets
SyncManager (and future modules like indexing, plugins) react to mutations
automatically.

**Design principles:**

- **Notifications, not cancellable hooks.** vxcore events are fire-and-forget.
  Cancellation/filtering belongs in the UI layer (Qt's `HookManager`).
- **Events are enqueued to the WorkQueue**, not dispatched inline. This keeps
  mutation paths fast and avoids re-entrancy issues.
- Event names are string constants (e.g., `"file.created"`, `"file.saved"`).
- Event data is a `nlohmann::json` object — same boundary format as the rest of vxcore.

**Architecture:**

```
FolderManager::CreateFile()
    │
    ▼
EventManager::Emit("file.created", {path, notebookId})
    │
    ▼
WorkQueue::Enqueue( [listeners, event] { for each listener: listener(event); } )
    │
    ▼
Worker thread calls ProcessNext() → listeners fire on worker thread
```

**Event names (v1):**

| Event | Payload | Fired from |
|-------|---------|------------|
| `file.created` | `{notebookId, path}` | `FolderManager::CreateFile` |
| `file.saved` | `{notebookId, path}` | `BufferManager::SaveBuffer` |
| `file.deleted` | `{notebookId, path}` | `FolderManager::DeleteFile` |
| `file.moved` | `{notebookId, oldPath, newPath}` | `FolderManager::MoveFile` |
| `folder.created` | `{notebookId, path}` | `FolderManager::CreateFolder` |
| `folder.deleted` | `{notebookId, path}` | `FolderManager::DeleteFolder` |
| `notebook.opened` | `{notebookId}` | `NotebookManager::OpenNotebook` |
| `notebook.closed` | `{notebookId}` | `NotebookManager::CloseNotebook` |

**C API:**

```c
typedef void (*VxCoreEventCallback)(const char *event_name,
                                     const char *json_data,
                                     void *userdata);

VXCORE_API VxCoreError vxcore_on_event(VxCoreContextHandle ctx,
                                        const char *event_name,
                                        VxCoreEventCallback callback,
                                        void *userdata);

VXCORE_API VxCoreError vxcore_off_event(VxCoreContextHandle ctx,
                                         const char *event_name,
                                         VxCoreEventCallback callback);
```

**SyncManager auto-sync integration:**

SyncManager registers itself as a listener for `file.*` events. On receiving an
event, it marks the notebook as dirty. The caller's periodic tick (or a debounce
timer) then triggers `TriggerSync()` for dirty notebooks.

```
file.saved event → SyncManager marks notebook dirty
                 → caller's timer fires → vxcore_sync_trigger(dirty_notebook)
```

This avoids syncing on every keystroke. The debounce/interval policy stays with
the caller, keeping vxcore simple.

**Files to create/modify:**

| Action | File |
|--------|------|
| Create | `src/core/event_manager.h` — `EventManager` class |
| Create | `src/core/event_manager.cpp` — implementation |
| Create | `src/core/event_names.h` — string constants |
| Create | `tests/test_event_manager.cpp` — subscribe, emit, unsubscribe |
| Modify | `src/core/context.h` — add `EventManager` |
| Modify | `src/core/bundled_folder_manager.cpp` — emit events |
| Modify | `src/core/raw_folder_manager.cpp` — emit events |
| Modify | `src/api/vxcore_work_queue_api.cpp` or new file — C API for events |
| Modify | `include/vxcore/vxcore.h` — declare event C API |
| Modify | `src/sync/sync_manager.h/.cpp` — register as event listener, dirty tracking |

**Tasks:**

- [x] P2-1: Implement `EventManager` (listener registry + emit via WorkQueue)
- [x] P2-2: Define event name constants in `event_names.h`
- [x] P2-3: Add C API (`vxcore_on_event`, `vxcore_off_event`)
- [x] P2-4: Fire events from `BundledFolderManager`
- [x] P2-5: Fire events from `NotebookManager` (open/close)
- [x] P2-6: SyncManager: register as listener, implement dirty-notebook tracking
- [x] P2-7: Tests — 18 tests (8 unit + 1 C API + 5 integration + 4 dirty-tracking)
- [x] P2-8: Update AGENTS.md

**Test plan (`tests/test_event_manager.cpp`):**

- [ ] T2-1: Subscribe to event, emit it, listener receives correct name + payload
- [ ] T2-2: Subscribe multiple listeners to same event, all fire
- [ ] T2-3: Subscribe to different events, only matching listener fires
- [ ] T2-4: Unsubscribe removes listener; subsequent emit does not fire it
- [ ] T2-5: Emit with no listeners is a no-op (no crash)
- [ ] T2-6: Listener receives valid JSON payload with expected fields
- [ ] T2-7: Events dispatched through WorkQueue (emit on thread A, listener fires on worker thread B)
- [ ] T2-8: C API roundtrip: `vxcore_on_event` + emit from FolderManager, callback fires
- [ ] T2-9: C API `vxcore_off_event` removes the callback

**Integration tests (`tests/test_event_sync_integration.cpp`):**

- [ ] T2-I1: Create file → `file.created` event fires → SyncManager marks notebook dirty
- [ ] T2-I2: Save buffer → `file.saved` event fires → SyncManager marks notebook dirty
- [ ] T2-I3: Delete file → `file.deleted` event fires → SyncManager marks notebook dirty
- [ ] T2-I4: Move file → `file.moved` event fires with oldPath and newPath
- [ ] T2-I5: Open/close notebook → `notebook.opened`/`notebook.closed` events fire
- [ ] T2-I6: Dirty notebook list cleared after sync trigger
- [ ] T2-I7: Multiple file ops on same notebook → only one dirty entry (no duplicates)

---

## Phase 3: Backend Genericity — Extensible Config & Credentials

**Why:** `SyncConfig` and `SyncCredentials` currently have Git-specific fields.
Adding WebDAV, S3, or Dropbox backends requires passing backend-specific options
without changing the struct for each new backend.

**Changes to `SyncConfig`:**

```cpp
struct SyncConfig {
  bool enabled = false;
  std::string backend;
  std::string remote_url;
  bool auto_sync_enabled = true;
  std::vector<std::string> exclude_paths = {"*.vswp", "vx_notebook/vx_sync/"};

  // NEW: opaque bag for backend-specific options.
  // Git: {} (no extra options needed)
  // WebDAV: {"auth_header": "Bearer ...", "depth": 1}
  // S3: {"bucket": "...", "region": "...", "prefix": "notebooks/"}
  nlohmann::json backend_options;
};
```

> Historical note: this field was originally the int `interval_seconds = 300`. It was renamed (clean break, no migration) to the boolean `auto_sync_enabled = true`, a pure on/off gate that suppresses `sync.should_run` when false. vxcore no longer carries any auto-sync cadence; the trailing-throttle cadence now lives Qt-side in `SyncService`, keyed off the global `autoSyncDebounceSeconds` value in `vxcore.json`.

**Changes to `SyncCredentials`:**

```cpp
struct SyncCredentials {
  std::string username;
  std::string password;
  std::string ssh_public_key_path;
  std::string ssh_private_key_path;
  std::string personal_access_token;
  std::string author_name;
  std::string author_email;

  // NEW: opaque bag for backend-specific credentials.
  // OAuth backends: {"access_token": "...", "refresh_token": "...", "expires_at": 1234}
  // S3: {"access_key_id": "...", "secret_access_key": "..."}
  nlohmann::json extra;
};
```

**JSON contract (C API):**

The `config_json` passed to `vxcore_sync_enable` gains an optional
`"backendOptions"` object. The `credentials_json` passed to
`vxcore_sync_set_credentials` gains an optional `"extra"` object. Both are
forwarded to the backend as-is; vxcore core does not interpret them.

**ISyncBackend interface — no change needed.** Backends already receive
`SyncConfig` and `SyncCredentials` in `Initialize()` and `SetCredentials()`.
They read `backend_options` / `extra` for their own needs.

**Files to modify:**

| Action | File |
|--------|------|
| Modify | `src/sync/sync_types.h` — add `backend_options` and `extra` fields |
| Modify | `src/sync/sync_types.cpp` (if exists) or inline — update `FromJson`/`ToJson` |
| Modify | `src/sync/git_sync_backend.cpp` — no-op (ignores `backend_options`) |
| Modify | `src/api/vxcore_sync_api.cpp` — pass through new JSON fields |
| Modify | `tests/test_sync.cpp` — roundtrip test for new fields |
| Modify | `src/sync/AGENTS.md` — document new fields |

**Tasks:**

- [x] P3-1: Add `backend_options` to `SyncConfig`, implement `FromJson`/`ToJson`
- [x] P3-2: Add `extra` to `SyncCredentials`
- [x] P3-3: Update C API JSON parsing to use `SyncConfig::FromJson` and pass through new fields
- [x] P3-4: Tests — 7 new tests in test_sync.cpp
- [x] P3-5: Update AGENTS.md and sync AGENTS.md

**Test plan (add to `tests/test_sync.cpp`):**

- [ ] T3-1: `SyncConfig::FromJson` with `backendOptions` populates `backend_options`
- [ ] T3-2: `SyncConfig::ToJson` includes `backendOptions` when non-empty
- [ ] T3-3: `SyncConfig` roundtrip with nested `backendOptions` object
- [ ] T3-4: `SyncConfig::FromJson` without `backendOptions` leaves it as null/empty (backward compat)
- [ ] T3-5: `SyncCredentials` with `extra` field roundtrips through JSON
- [ ] T3-6: C API: `vxcore_sync_enable` with `backendOptions` in JSON, verify via `vxcore_sync_get_status` or `GetSyncConfig`
- [ ] T3-7: Git backend ignores `backend_options` gracefully (no crash, no behavior change)
- [ ] T3-8: Mock backend receives and can read `backend_options` from config

---

## Summary

| Phase | What | Depends On | Key Deliverable |
|-------|------|-----------|-----------------|
| 1 | Generic WorkQueue | — | `src/core/work_queue.h`, C API |
| 2 | Event System | Phase 1 | `EventManager`, auto-dirty for sync |
| 3 | Extensible Config | — (can parallel Phase 2) | `backend_options`, `extra` JSON bags |
