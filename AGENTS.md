# Agent Guidelines for vxcore

## Project Overview

vxcore is a cross-platform C/C++ library providing notebook management functionality for VNote. It offers a stable C ABI for embedding in desktop (Qt), iOS (Swift), and Android (Kotlin) applications.

**Key Features**: Notebook management, folder/file operations, tag system, full-text search, JSON-based configuration.

## Library Integration Contract

vxcore is an **embedded library**, not an application. It publishes a stable C ABI; consumers (VNote is one) integrate by following the published contract. The library owns *facts*; consumers own *policy*.

### The 4-layer model

| Layer | Owner | Job | Forbidden |
|---|---|---|---|
| 1. Event emission | vxcore (FolderManager, BufferManager, NotebookManager) | Fire faithful, lossless state-change events on every mutation. | Deciding when consumers act. Throttling, debouncing, or filtering events. |
| 2. Dirtiness tracking | vxcore (DirtyTracker, per-notebook flag) | Track outstanding work; cleared on TriggerSync success. | Scheduling sync. Applying timing policy. |
| 3. Scheduling policy | CONSUMER (e.g., VNote SyncWorkQueueManager + SyncScheduler) | Decide WHEN sync runs: debounce, throttle, coalesce, backpressure, retry, periodic safety net. | Touching git, libgit2, or backend internals. |
| 4. Sync execution | vxcore (SyncManager::TriggerSync + GitSyncBackend) | Synchronously execute the round-trip; return a result; fire lifecycle events. | Knowing about scheduling. Embedding timers. |

### Anti-patterns (FORBIDDEN)

Three recent regressions all violated the contract. Do not repeat them.

1. **Static-initializer self-registration of backends.** Use the explicit `RegisterBuiltinBackends(registry)` call from `vxcore_context_create`. Anchor functions, dead-strip workarounds, and cross-TU pointer tricks are forbidden. The MSVC `/OPT:REF` linker silently dropped the old self-registering translation unit out of `vnote.exe`. Fixed in commit `fff4a5f`.
2. **Vxcore-side scheduling decisions** (debounce gates, throttle, deferred timers inside `MaybeEnqueueSync`). Vxcore emits facts; consumers schedule. The previous in-library debounce swallowed legitimate `sync.should_run` events when several saves arrived in the same window. Fixed in commit `cba4e21`.
3. **Implicit propagation in setters.** When a setter alters state that is already wired into sub-objects, it MUST iterate and propagate explicitly. `NotebookManager::SetEventManager` previously stored the pointer but did not push it into already-loaded notebooks, so folder events from those notebooks went nowhere. Fixed in commit `144ca4e`.
4. **Re-introducing filter logic inside emission paths.** No "skip if recent", no "emit every Nth call", no per-listener throttling inside `EmitEvent` / `EventManager::Emit` / `EventManager::EmitAsync`. Consumers own all such policy. The May 2026 audit (A9) verified emission paths are lossless; do not regress this.

### Migration notes for future backends

- New backends register via `RegisterBuiltinBackends`. Never add static initializers, `__attribute__((constructor))`, or file-scope objects with side-effecting constructors.
- Never emit scheduling-policy events from vxcore. If a backend wants to "ask" for a sync, mark dirty and emit `sync.should_run`; let the consumer decide.
- Backends must not own timers, threads, or schedulers. Phase methods are synchronous; the consumer owns the worker.

### Known event-protocol gaps (sync-architecture-audit, May 2026)

The May 2026 sync-architecture-audit (`docs/plans/sync-architecture-audit.md`, A9) confirmed the emission paths themselves are lossless — no debounce/throttle/counter filtering remains inside `EmitEvent` or `EventManager::Emit`. It also surfaced two pre-existing protocol gaps. The `vxcore-metadata-events` plan (June 2026) closed the bulk of the second gap; the remaining items below should still close before event-driven sync becomes the only reliable path for the affected code:

1. **`RawFolderManager` emits no events at all.** The raw-notebook code path is structurally unwired from the event bus: every mutation method returns from its success path without calling `EmitEvent`.

   Affected raw ops cover both the file/folder structural mutations and the metadata-write ops: `CreateFolder`, `DeleteFolder`, `RenameFolder`, `MoveFolder`, `CopyFolder`, `CreateFile`, `DeleteFile`, `RenameFile`, `MoveFile`, `CopyFile`, `ImportFile`, `ImportFolder`, `UpdateFileMetadata`, `UpdateFileTags`, `TagFile`, `UntagFile`. Auto-sync for raw notebooks therefore cannot rely on `sync.should_run` today; the polling fallback `GetDirtyNotebooks()` is the only reliable signal. Wiring raw notebooks into the event bus is deferred to a separate plan (raw notebooks do not participate in sync today, so this is not blocking).

2. **Bundled structural ops without dedicated events.** The following `BundledFolderManager` mutations have no event-name constant of their own: `RenameFolder`, `MoveFolder`, `CopyFolder`, `CopyFile`, `ImportFile`, `ImportFolder`. These ops ARE covered by `folder.config_changed` for auto-sync purposes — every write to a folder's `vx.json` flags the notebook dirty, so `SyncManager::mark_dirty` does wake on them (see `test_rename_folder_triggers_sync_chain` in `tests/test_sync.cpp`). They still lack a dedicated structural event (`folder.moved`, `folder.copied`, `file.copied`, `file.imported`, `folder.imported`) for richer UI dispatch, which is deferred to a follow-up plan.

   The `vxcore-metadata-events` plan (June 2026) closed the nine prior metadata-write items in this lane by adding event-name constants in `src/core/event_names.h` and wiring emit calls into the success branch of each mutation. Newly emitting methods: `BundledFolderManager::TagFile` → `file.tagged`, `UntagFile` → `file.untagged`, `UpdateFileTags` → `file.tags_replaced`, `UpdateFileMetadata` → `file.metadata_updated`, `UpdateFolderMetadata` → `folder.metadata_updated`, `AddFileAttachment` → `file.attached`, `DeleteFileAttachment` → `file.detached`, `UpdateFileAttachments` → `file.attachments_replaced`, and `NotebookManager::UpdateNotebookConfig` (via `BundledNotebook::UpdateConfig`) → `notebook.config_changed`. The plan also added `folder.config_changed` from `SaveFolderConfig`, which is the persistence event that covers the structural ops above for auto-sync.

### ABI versioning (no tags yet)

The vxcore submodule currently has **no git tags**, so there is no formal release baseline. Until that changes:
- Treat the `!` convention in commit subjects (e.g., `refactor(vxcore)!: ...`) as the source of truth for breaking C-ABI changes.
- Enum additions MUST be appended before the `VXCORE_ERR_UNKNOWN = 999` sentinel with an explicit value; never reorder existing enumerators.
- Cut `v0.1.0` at the next stable point and adopt SemVer thereafter (MAJOR for `!` commits, MINOR for pure additions, PATCH for internal fixes).

## Logging API

vxcore writes log lines through an internal `Logger` (see `src/utils/logger.h`). By default the logger fans messages out to stderr and, if configured, to a rotating file. Embedders that want to capture those messages and route them through their own logging system (e.g., Qt's `qInstallMessageHandler` pipeline in VNote) can install a custom handler via the C ABI declared in `include/vxcore/vxcore_log.h`.

### Public entry point

```c
typedef void (*VxCoreLogCallback)(VxCoreLogLevel level,
                                  const char *file,
                                  int line,
                                  const char *message,
                                  void *userdata);

VXCORE_API VxCoreError vxcore_log_set_handler(VxCoreLogCallback callback,
                                               void *userdata);
```

Callback parameters:

| Parameter | Meaning |
|---|---|
| `level` | One of `VXCORE_LOG_LEVEL_TRACE`..`VXCORE_LOG_LEVEL_FATAL` (never `OFF`). |
| `file` | Source file of the originating `VXCORE_LOG_*` call (`__FILE__`). Valid only for the call's duration. |
| `line` | Source line number (`__LINE__`) of the originating call. |
| `message` | Null-terminated, already-formatted message text. Valid only for the call's duration. |
| `userdata` | The opaque pointer supplied at `vxcore_log_set_handler` time. |

### Contract

- **Replaces, not adds.** Installing a handler bypasses the default stderr/file sinks for the duration of the install. The library will not also write to its built-in sinks while a custom handler is active.
- **Uninstall with NULL.** Passing `nullptr` as the callback restores the default sinks. `userdata` is ignored in that case.
- **Held under the logger mutex.** The callback runs while vxcore's internal logger mutex is held. It MUST be thread-safe AND MUST NOT call back into any vxcore API synchronously, doing so deadlocks the logger. Queue work onto another thread if you need to invoke vxcore again.
- **Pointer lifetime.** `file` and `message` are stack/temporary buffers from vxcore's perspective. Copy them before storing them past the callback return.

### Example

```c
static void my_log(VxCoreLogLevel level, const char *file, int line,
                   const char *message, void *userdata) {
  (void)userdata;
  fprintf(stderr, "[vxcore %d] %s:%d %s\n", (int)level, file, line, message);
}

vxcore_log_set_handler(my_log, NULL);
/* ... run app ... */
vxcore_log_set_handler(NULL, NULL);  /* restore default sinks */
```

### Environment variables

The logger supports configuration through environment variables:

| Variable | Values | Default | Behavior |
|---|---|---|---|
| `VXCORE_LOG_LEVEL` | `trace`, `debug`, `info`, `warn`, `error`, `fatal`, `off` (case-insensitive) | `info` | Controls minimum log level. Invalid values silently fall back to default. |
| `VXCORE_LOG_CONSOLE` | `0`, `false`, `off` (case-insensitive) for disable; anything else for enable | `1` (enabled) | Controls whether log messages are written to stderr. Release builds should set `=0` when installing a custom callback via `vxcore_log_set_handler`. |
| `VXCORE_LOG_FILE` | File path | (none) | If set, log messages are also written to the specified file (appended). |

**Examples:**
```bash
# Enable debug-level logging to console and stderr
export VXCORE_LOG_LEVEL=debug
export VXCORE_LOG_CONSOLE=1

# Disable console output (useful when using vxcore_log_set_handler in production)
export VXCORE_LOG_CONSOLE=0

# Write logs to a file
export VXCORE_LOG_FILE=/var/log/vxcore.log

# Trace-level logging for troubleshooting (Unix)
VXCORE_LOG_LEVEL=trace VXCORE_LOG_FILE=/tmp/vxcore_debug.log ./myapp

# PowerShell (Windows)
$env:VXCORE_LOG_LEVEL="trace"; $env:VXCORE_LOG_FILE="C:\Temp\vxcore.log"; .\myapp.exe
```

## Build Commands

```bash
# Configure
cmake -B build -DVXCORE_BUILD_TESTS=ON

# Build
cmake --build build

# Run all tests
ctest --test-dir build -C Debug     # Windows
ctest --test-dir build              # Unix

# Run single test module
ctest --test-dir build -C Debug -R test_core
ctest --test-dir build -C Debug -R test_notebook
ctest --test-dir build -C Debug -R test_folder
ctest --test-dir build -C Debug -R test_search
ctest --test-dir build -C Debug -R test_db

# Run with verbose output
ctest --test-dir build -C Debug -V
```

## Test Structure

- Each module has its own test executable: `test_core`, `test_notebook`, `test_folder`, `test_search`, `test_db`, etc.
- Individual test cases tracked within each module's `main()`
- Test macros defined in `tests/test_utils.h`: `ASSERT`, `ASSERT_EQ`, `ASSERT_NE`, `ASSERT_TRUE`, `ASSERT_FALSE`, `ASSERT_NULL`, `ASSERT_NOT_NULL`, `RUN_TEST`
- **Test Mode**: All tests call `vxcore_set_test_mode(1)` to isolate test data
  - Enabled: Uses `%TEMP%\vxcore_test` (Windows) or `/tmp/vxcore_test` (Unix)
  - Disabled: Uses real AppData paths (`%APPDATA%\VNote`, `%LOCALAPPDATA%\VNote`)
  - CLI tools do NOT enable test mode (use production paths)

### Test binary build patterns

Two CMake patterns coexist in `libs/vxcore/tests/CMakeLists.txt`:

| Pattern | Helper | When to use |
|---|---|---|
| **Link via `vxcore.dll`** | `add_vxcore_test(test_name)` | Tests that only exercise the public C API (`vxcore_*` functions). The helper compiles `test_name.cpp`, links `vxcore`, and registers the ctest entry. |
| **Direct-compile** | Manual `add_executable(...)` listing the under-test `.cpp` as a source | Tests that call C++-internal symbols not decorated with `VXCORE_API`. **All `GitSyncBackend` methods fall in this bucket** — `Initialize`, `Sync`, `Push`, etc. are not exported. Using `add_vxcore_test` for these yields `LNK2019` unresolved-external errors. |

**Transitive-utility rule (CRITICAL for direct-compile tests):** when a test compiles `src/sync/git_sync_backend.cpp` (or any other internal .cpp) directly, it MUST also compile every utility `.cpp` that source transitively references. The `git_sync_test_helpers` STATIC library bundles `libgit2_init.cpp` + `logger.cpp` + `file_utils.cpp` — do NOT add those again (ODR violations), but anything else (e.g., `string_utils.cpp` for `vxcore::MatchesPatterns`) must be added explicitly to every affected target.

Whenever you add a new `utils/*.h` call from inside `git_sync_backend.cpp` (or similar shared sources), audit EVERY `add_executable` block in `tests/CMakeLists.txt` that lists that source — each one needs the new utility `.cpp` added or it fails to link with `LNK2019`. The `test_gitkeep_*` plan caught this lesson the hard way: T7 wired `EnsureGitkeepFiles()` into `Sync()`/`Push()` but only updated 3 of 8 affected test targets; the remaining 5 (`test_git_sync_init`/`credentials`/`status`/`conflicts`/`pushpull`) needed a follow-up commit to compile from clean.

### Test isolation on Windows

- All vxcore tests share `%TEMP%\vxcore_test_data` + `%TEMP%\vxcore_test_config`. When run as the full ctest suite, leftover state plus Windows file-locks cascade: tests like `test_notebook`, `test_folder`, `test_history`, `test_template` commonly fail in the full suite but PASS individually after a clean temp dir.
- The full-suite failures are NOT regressions per se — verify by running the failing test alone after `Remove-Item "$env:TEMP\vxcore_test_data" -Recurse -Force` + `Remove-Item "$env:TEMP\vxcore_test_config" -Recurse -Force`.
- **Do NOT wipe the `vxcore_test_data` parent directory inside a QA script.** libgit2's `GIT_REPOSITORY_INIT_MKPATH` does not reliably create grandparent directories on Windows in this environment; libgit2-backed tests (including `test_git_sync_roundtrip` and the `test_gitkeep_*` set) rely on the parent persisting between runs. If you must clean state, only wipe leaf subdirectories.

### Standalone test pattern (tests touching libgit2 / GitSyncBackend)

For tests that drive `GitSyncBackend` end-to-end via libgit2 (e.g., `test_git_sync_*`, `test_gitkeep_*`):

- `main()` must start with `vxcore_set_test_mode(1);` to redirect AppData → `%TEMP%`.
- Tests that do NOT link `vxcore.dll` (because they direct-compile internal sources) must provide a local anonymous-namespace no-op shim for `vxcore_set_test_mode` so the call links — see `test_gitkeep_basic.cpp` for the canonical shim (a `void vxcore_set_test_mode(int) {}` stub in an anonymous namespace).
- Link the `git_sync_test_helpers` STATIC library (defined in `tests/CMakeLists.txt`) for the libgit2 fixture API: `create_bare_repo(path) -> std::string`, `commit_file(...)`, `git_head_sha(...)`, `git_status_clean(...)`.
- **NEVER call libgit2's `git_clone()` for local `file://` remotes on Windows** — it hangs. Use the init+remote+fetch+checkout sequence instead: `git_repository_init_ext` → `git_remote_create("origin", url)` → `git_remote_fetch` → `git_reference_lookup("refs/remotes/origin/main")` → `git_reference_peel` → `git_checkout_tree(FORCE)` → `git_reference_create("refs/heads/main")` → `git_repository_set_head`. See `clone_bare_to_workdir` in `test_gitkeep_roundtrip.cpp` for the canonical implementation; existing comments in `test_git_sync_init.cpp` and `test_git_sync_helpers.cpp` document the rationale.

## Code Style

- **Language**: C++17 with C ABI for public API
- **Formatting**: Use `.clang-format` based on Google style (indent 2 spaces, 100 col limit, pointer alignment right)
- **Headers**: Sort includes; public API in `include/vxcore/`, implementation in `src/`
- **File Format**: Unix line endings

### Naming Conventions (Google C++ Style Guide)

| Element | Convention | Examples |
|---------|------------|----------|
| C API functions | `snake_case` | `vxcore_context_create`, `vxcore_notebook_open` |
| Types (classes/structs/enums) | `PascalCase` | `NotebookManager`, `VxCoreError`, `FolderConfig` |
| Functions/Methods | `PascalCase` | `GetNotebook()`, `CreateFolder()`, `ToJson()` |
| Variables (local/global) | `snake_case` | `notebook_id`, `root_folder`, `out_config_json` |
| Class data members | `snake_case_` (trailing underscore) | `config_`, `notebooks_`, `folder_manager_` |
| Struct data members | `snake_case` (no trailing underscore) | `assets_folder`, `created_utc`, `parent_id` |
| Constants | `kPascalCase` | `kMaxPathLength`, `kConfigFileName` |
| Enumerators | `kPascalCase` | `kSuccess`, `kTrace`, `kDebug` |
| Macros | `UPPER_CASE` | `VXCORE_API`, `RUN_TEST`, `VXCORE_LOG_INFO` |
| Namespaces | `snake_case` | `vxcore`, `vxcore::db` |

### JSON Conventions

- **JSON keys**: Always use `camelCase` (e.g., `createdUtc`, `modifiedUtc`, `assetsFolder`, `rootFolder`)
- **C++ struct members**: Use `snake_case` (e.g., `created_utc`, `modified_utc`, `assets_folder`)
- **Rationale**: User-facing files (config.json, session.json) follow JavaScript conventions

> **⚠ JSON Key Mismatch Warning**
>
> When sending or receiving JSON to/from vxcore, you **must** use the exact camelCase keys defined in the corresponding `FromJson()` / `ToJson()` methods. Do **not** invent similar-sounding names.
>
> **Real-world example**: Qt code sent `pathPatterns` but vxcore parses `filePatterns` — the field was silently ignored and search filters stopped working.
>
> Rules:
> 1. Before writing or changing any JSON key on the Qt/controller side, open the relevant vxcore `FromJson()` / `ToJson()` source and verify the exact key string.
> 2. Never assume a key name from context; always confirm against the parser code.
> 3. When renaming a JSON key in vxcore, update **all** callers (Qt services, CLI, tests) in the same change.
> 4. Update tests alongside any JSON contract change to catch mismatches at build time.

> **SSOT for shared JSON keys (post-2026-06 lift-shared-json-keys plan)**
>
> All JSON keys that cross the vxcore↔consumer boundary now live in a SINGLE
> public header: [`include/vxcore/notebook_json_keys.h`](include/vxcore/notebook_json_keys.h).
> Both vxcore (writer) AND Qt-side / plugin (reader) code include this header
> and reference the SAME `kJsonKey*` constants. Drift between writer and
> reader is structurally impossible.
>
> Sync-related JSON keys live in their own header:
> [`src/sync/sync_json_keys.h`](src/sync/sync_json_keys.h)
> (currently internal — promote to public via the same pattern if a Qt-side
> consumer ever needs them).
>
> **A grep-gate ctest enforces the pattern**: `tests/utils/test_json_key_drift.cpp`
> in the parent VNote project scans `src/` for inline literals from the
> gated set (`"rootFolder"`, `"assetsFolder"`, `"tagsModifiedUtc"`,
> `"notebookId"`, `"isReadOnly"`, `"targetDir"`, `"readOnly"`) and FAILS
> the build if any are found outside the allow-list of Qt-internal-schema
> files (workspace state, sessionconfig, etc.).
>
> **Historical trigger**: commit
> [`13680f8d`](https://github.com/vnotex/vnote) ("fix(controllers): use
> canonical rootFolder JSON key") fixed a SILENT 4-site bug where Qt-side
> controllers read `"root_path"` while vxcore wrote `"rootFolder"`. The
> duplicate-open guards never matched: pure logic drift masked by safe
> defaults. The SSOT lift eliminates the entire bug class.
>
> **Adding a new shared-schema JSON key**:
>
> 1. Add the constant to `include/vxcore/notebook_json_keys.h`
>    (or `src/sync/sync_json_keys.h` for sync keys). Constants
>    are additive only — NEVER rename or delete (public ABI contract).
> 2. Use the constant in both the writer (vxcore C++) and reader (vxcore +
>    Qt-side C++).
> 3. NEVER inline the literal in production code.
> 4. The grep-gate test will catch violations at build time.
>
> **Adding a Qt-internal schema that legitimately uses a gated literal**:
> add the file to the `kAllowedFiles` list in `tests/utils/test_json_key_drift.cpp`
> with a one-line reason. Do NOT use the per-line `// json-key-drift-allow`
> escape hatch as a permanent solution.

### Error Handling

- Return `VxCoreError` codes from C API functions
- Check null pointers; use `(void)param` for unused parameters
- Caller frees strings with `vxcore_string_free()`
- Context owns handles; destroy context to free all resources

### Platform

- Use `VXCORE_API` macro for exports; Windows DLL-safe
- No Qt dependencies in core library
- Namespaces: C++ code in `namespace vxcore`; close with `// namespace vxcore`

## Folder Structure

```
vxcore/
├── include/vxcore/          # Public C API headers
│   ├── vxcore.h             # Main API (context, notebook, folder, file, tag, search)
│   ├── vxcore_types.h       # Error codes, handles, version struct
│   └── vxcore_log.h         # Logging API
│
├── src/
│   ├── api/                 # C API implementation layer
│   │   ├── vxcore_api.cpp           # Context, version, test mode
│   │   ├── vxcore_notebook_api.cpp  # Notebook operations
│   │   ├── vxcore_folder_api.cpp    # Folder/file operations
│   │   ├── vxcore_tag_api.cpp       # Tag operations
│   │   ├── vxcore_search_api.cpp    # Search operations
│   │   ├── vxcore_sync_api.cpp      # Sync operations
│   │   ├── vxcore_event_api.cpp     # Event system C API
│   │   ├── vxcore_work_queue_api.cpp # Work queue C API
│   │   ├── api_utils.h              # API helper utilities
│   │   ├── error_handler.h/.cpp     # Error handling utilities
│   │   └── handle_manager.h/.cpp    # Handle management
│   │
│   ├── core/                # Core C++ business logic
│   │   ├── context.h                # VxCoreContext: owns ConfigManager + NotebookManager
│   │   ├── config_manager.h/.cpp    # VxCoreConfig + VxCoreSessionConfig management
│   │   ├── vxcore_config.h/.cpp     # Application config (vxcore.json)
│   │   ├── vxcore_session_config.h/.cpp  # Session config (session.json)
│   │   ├── notebook_manager.h/.cpp  # Notebook lifecycle (create, open, close, list)
│   │   ├── notebook.h/.cpp          # Notebook base class, NotebookConfig, NotebookRecord, TagNode
│   │   ├── bundled_notebook.h/.cpp  # Bundled notebook (config in vx_notebook/config.json)
│   │   ├── raw_notebook.h/.cpp      # Raw notebook (config in session only)
│   │   ├── folder_manager.h         # Abstract folder/file operations interface
│   │   ├── bundled_folder_manager.h/.cpp  # Folder ops for bundled notebooks
│   │   ├── raw_folder_manager.h/.cpp      # Folder ops for raw notebooks
│   │   ├── folder.h/.cpp            # FolderConfig, FolderRecord, FileRecord
│   │   ├── event_manager.h/.cpp     # Event system (subscribe/emit notifications)
│   │   ├── event_names.h            # Event name constants (file.created, etc.)
│   │   └── work_queue.h/.cpp        # Thread-safe work queue + named queue manager
│   │
│   ├── db/                  # SQLite database layer
│   │   ├── db_manager.h/.cpp        # Database lifecycle, schema, transactions
│   │   ├── db_schema.h              # Schema definitions (tables, indexes, FTS5)
│   │   ├── file_db.h/.cpp           # File/folder CRUD operations
│   │   ├── tag_db.h/.cpp            # Tag CRUD and queries
│   │   └── sync_manager.h/.cpp      # Filesystem ↔ database synchronization
│   │
│   ├── search/              # Search subsystem
│   │   ├── search_manager.h/.cpp    # Search orchestration
│   │   ├── search_backend.h         # ISearchBackend interface
│   │   ├── simple_search_backend.h/.cpp  # Built-in search implementation
│   │   ├── rg_search_backend.h/.cpp      # Ripgrep integration
│   │   ├── search_query.h/.cpp      # SearchQuery, SearchScope, SearchOption
│   │   └── search_file_info.h/.cpp  # SearchFileInfo struct
│   │
│   ├── sync/                # Sync backend subsystem (see src/sync/AGENTS.md)
│   │   ├── sync_types.h             # Sync enums, structs, callback typedef
│   │   ├── sync_backend.h           # ISyncBackend pure virtual interface
│   │   ├── sync_manager.h/.cpp      # SyncManager orchestrator (per-notebook dispatch)
│   │   └── AGENTS.md               # Detailed sync module documentation
│   │
│   ├── platform/            # Platform-specific code
│   │   ├── path_provider.h/.cpp     # AppData, LocalData paths per platform
│   │   └── process_utils.h/.cpp     # External process execution (for rg)
│   │
│   └── utils/               # General utilities
│       ├── utils.h/.cpp             # GenerateUUID(), GetCurrentTimestampMillis(), HasFlag()
│       ├── file_utils.h/.cpp        # CleanPath(), ConcatenatePaths(), LoadJsonFile()
│       ├── string_utils.h/.cpp      # ToLowerString(), MatchesPattern(), exclude patterns
│       └── logger.h/.cpp            # VXCORE_LOG_* macros, Logger singleton
│
├── cli/                     # Command-line interface
│   ├── main.cpp             # Entry point, command dispatch
│   ├── args.h/.cpp          # Argument parsing
│   ├── notebook_cmd.h/.cpp  # notebook subcommand
│   ├── tag_cmd.h/.cpp       # tag subcommand
│   ├── config_cmd.h/.cpp    # config subcommand
│   └── json_helpers.h/.cpp  # JSON formatting utilities
│
├── tests/                   # Test executables
│   ├── test_utils.h         # Test macros and helpers
│   ├── test_core.cpp        # Context, config tests
│   ├── test_notebook.cpp    # Notebook CRUD tests
│   ├── test_folder.cpp      # Folder/file operation tests
│   ├── test_search.cpp      # Search tests
│   ├── test_db.cpp          # Database layer tests
│   ├── test_sync.cpp        # Sync backend tests
│   └── ...
│
├── third_party/             # External dependencies (DO NOT MODIFY)
│   ├── nlohmann/json.hpp    # JSON library
│   └── sqlite/              # SQLite amalgamation
│
└── data/
    └── vxcore.json          # Default configuration template
```

## Architecture Notes

### Layered Architecture

```
┌─────────────────────────────────────────────────┐
│  C API Layer (include/vxcore/, src/api/)        │  ← Stable ABI for FFI
├─────────────────────────────────────────────────┤
│  Core Layer (src/core/)                         │  ← Business logic
├─────────────────────────────────────────────────┤
│  Database Layer (src/db/)                       │  ← SQLite metadata
├──────────────────────┬──────────────────────────┤
│  Platform (src/platform/)  │  Utils (src/utils/)│  ← Cross-cutting concerns
└──────────────────────┴──────────────────────────┘
```

### Key Design Patterns

1. **Context-based API**: All operations go through `VxCoreContextHandle`
2. **Opaque handles**: C API uses opaque pointers for type safety
3. **JSON boundaries**: All complex data crosses C ABI as JSON strings
4. **Two notebook types**:
   - **Bundled**: Config stored in `<root>/vx_notebook/config.json`, metadata in `<root>/vx_notebook/`
   - **Raw**: Config stored in session config only, metadata in local data folder
5. **FolderManager abstraction**: `BundledFolderManager` and `RawFolderManager` implement different storage strategies
6. **Search backends**: Pluggable via `ISearchBackend` interface (ripgrep, simple built-in)
7. **Sync backends**: Pluggable via `ISyncBackend` interface (Git, WebDAV, etc. — see `src/sync/AGENTS.md`)
8. **Event system**: `EventManager` provides fire-and-forget notifications (not cancellable). Events are emitted by `FolderManager` and `NotebookManager` on mutations. `SyncManager` subscribes to mark notebooks dirty for auto-sync.
9. **Work queues**: `WorkQueueManager` provides named thread-safe queues. vxcore does NOT create threads — the caller owns worker threads and calls `vxcore_work_queue_process_next()`.

### Thread Safety

- `NotebookManager` operations are NOT thread-safe; caller must synchronize
- `DbManager`, `FileDb`, `TagDb` are NOT thread-safe; designed for single-threaded use per notebook
- `WorkQueue` is thread-safe (mutex + condvar) — safe cross-thread boundary
- `EventManager::Emit()` is thread-safe (copies listener list under lock, then invokes outside lock)
- `SyncManager` dirty tracking (`GetDirtyNotebooks`/`ClearDirty`) is thread-safe (own mutex)
- `Logger` is thread-safe (uses mutex)

### Data Flow

```
User Code
    │
    ▼
C API (vxcore_*)
    │
    ▼
VxCoreContext
    ├── ConfigManager (VxCoreConfig, VxCoreSessionConfig)
    ├── EventManager (subscribe/emit notifications)
    ├── WorkQueueManager (named thread-safe work queues)
    ├── SyncManager (per-notebook ISyncBackend dispatch, dirty tracking via events)
    └── NotebookManager
            └── Notebook (Bundled/Raw)
                    ├── NotebookConfig (id, name, tags, metadata)
                    ├── FolderManager (folder/file operations, emits events)
                    │       └── DbManager + FileDb + TagDb
                    └── SearchManager (search operations)
                            └── ISearchBackend (rg/simple)
```

## Important Implementation Details

### Notebook Types

| Aspect | Bundled | Raw |
|--------|---------|-----|
| Config location | `<root>/vx_notebook/config.json` | Session config only |
| Metadata folder | `<root>/vx_notebook/` | `<local_data>/notebooks/<id>/` |
| Database location | `<local_data_folder>/notebooks/<id>/metadata.db` (see notebook.cpp:167-169) | `<local_data_folder>/notebooks/<id>/metadata.db` (see notebook.cpp:167-169) |
| Portable | Yes (self-contained) | No (external metadata) |

### Path Handling

- Always use `CleanPath()` or `CleanFsPath()` to normalize paths
- Paths use forward slashes internally (even on Windows)
- Relative paths within notebook are relative to notebook root
- Use `ConcatenatePaths()` to join path components

### UTF-8 Path Safety (CRITICAL on Windows)

> **⚠ NEVER pass `std::string` directly to `std::filesystem` functions.**
>
> On Windows, `std::filesystem` interprets `std::string` using the active ANSI code page, NOT UTF-8.
> Paths with non-ASCII characters (Chinese, Japanese, accented, etc.) will silently fail.

**Rules:**

| ❌ WRONG | ✅ CORRECT |
|----------|-----------|
| `std::filesystem::exists(utf8_string)` | `PathExists(utf8_string)` |
| `std::filesystem::is_directory(utf8_string)` | `IsDirectory(utf8_string)` |
| `std::filesystem::is_regular_file(utf8_string)` | `IsRegularFile(utf8_string)` |
| `std::filesystem::path p(utf8_string); p.filename().string()` | `PathFilename(utf8_string)` |
| `std::filesystem::exists(PathFromUtf8(x))` | `PathExists(x)` (preferred) |

**For code inside the DLL** (src/): Use `PathExists()`, `IsDirectory()`, `IsRegularFile()`, `PathFilename()` from `utils/file_utils.h`.

**For code outside the DLL** (cli/, tests/): Must provide a local `Utf8PathExists()` helper using `MultiByteToWideChar` + `#ifdef _WIN32`. See `cli/config_cmd.cpp` for the pattern.

**When you MUST use raw `std::filesystem`** (e.g., `create_directories`, `remove`, `rename`): Always wrap the path argument with `PathFromUtf8()`:
```cpp
std::filesystem::create_directories(PathFromUtf8(utf8_path));
std::filesystem::remove(PathFromUtf8(utf8_path));
std::filesystem::rename(PathFromUtf8(old_path), PathFromUtf8(new_path));
```

### Utility Functions (check before implementing)

- `src/utils/utils.h`: `GenerateUUID()`, `GetCurrentTimestampMillis()`, `HasFlag()`
- `src/utils/file_utils.h`: `CleanPath()`, `LoadJsonFile()`, `ConcatenatePaths()`, `SplitPath()`, `RelativePath()`
- `src/utils/string_utils.h`: `ToLowerString()`, `MatchesPattern()`, `MatchesPatterns()`
- `src/utils/logger.h`: `VXCORE_LOG_TRACE/DEBUG/INFO/WARN/ERROR/FATAL`

### Buffer external-change detection (`Buffer::CheckExternalChanges`)

`Buffer::CheckExternalChanges` (`src/core/buffer.cpp`) decides whether an open buffer's
file was modified on disk by an external tool. It is **content-aware, not mtime-only**:
a bare `last_write_time` change is NOT sufficient to report `VXCORE_BUFFER_FILE_CHANGED`.

Algorithm: if `current_mtime == last_modified_time_` → NORMAL. Otherwise compare on-disk
bytes against the cached `content_` — exact byte match first, then EOL-normalized
(`NormalizeEol`, CRLF/CR → LF) — via `FileContentMatchesBuffer`. On match it is a benign
mtime bump (VNote's own worker-thread save re-stamp, the git sync backend rewriting the
working tree with identical content during checkout/rebase, Windows lazy metadata flush,
AV/cloud-sync touches): refresh `last_modified_time_` to `current_mtime` and stay NORMAL.
Only a real content difference sets `FILE_CHANGED`, and the stamp is refreshed **only** on
confirmed equality so a genuine unresolved external edit keeps flagging on every check.

**Thread-safety contract (consumer-owned):** `content_` is read here without a lock, by
design — vxcore `Buffer` has no per-buffer mutex (adding one was rejected; see
`src/core/services/buffersavequeue.cpp` header). The Qt consumer MUST NOT call this while a
save is mutating the same buffer; VNote enforces this by skipping the check when
`BufferSaveQueue::isBusy(notebookId, bufferId)` (see VNote's
`src/core/services/AGENTS.md` § External-change detection gate). Keep this method free of
scheduling/timing policy — it reports a FACT (changed vs. not), the consumer owns WHEN to
check. Coverage: `tests/test_buffer.cpp` (`test_buffer_external_change_content_aware`,
`test_buffer_mtime_no_jitter`, `test_buffer_check_external_changes`).

### Logging

```cpp
#include "utils/logger.h"

VXCORE_LOG_INFO("Opening notebook: %s", path.c_str());
VXCORE_LOG_ERROR("Failed to open database: %s", db_path.c_str());
```

## Common Patterns

### Adding a New C API Function

1. Declare in `include/vxcore/vxcore.h`
2. Implement in appropriate `src/api/vxcore_*_api.cpp`
3. Add tests in `tests/test_*.cpp`

### Adding a New Core Feature

1. Implement in `src/core/`
2. Expose through existing managers or create new manager
3. Add C API wrapper in `src/api/`
4. Add tests

### Working with JSON

```cpp
#include <nlohmann/json.hpp>

// Parsing
nlohmann::json j = nlohmann::json::parse(json_string);
auto config = NotebookConfig::FromJson(j);

// Serializing
nlohmann::json j = config.ToJson();
std::string json_string = j.dump();
```

### Error Handling Pattern

```cpp
VxCoreError MyFunction(const char* input, char** output) {
  if (!input || !output) {
    return VXCORE_ERR_NULL_POINTER;
  }

  try {
    // ... implementation ...
    *output = strdup(result.c_str());
    return VXCORE_OK;
  } catch (const std::exception& e) {
    VXCORE_LOG_ERROR("MyFunction failed: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }
}
```

### Two-pass directory walker (`recursive_directory_iterator`)

When you need to mutate the filesystem during a recursive traversal, **never** mutate during iteration — `std::filesystem::recursive_directory_iterator` can invalidate. Always collect into a `std::vector<fs::path>` first, then mutate in a second pass:

```cpp
std::error_code ec;
std::vector<std::filesystem::path> to_modify;

// Pass 1: collect.
try {
  for (auto it = std::filesystem::recursive_directory_iterator(
           root, std::filesystem::directory_options::skip_permission_denied, ec);
       it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    if (ec) { VXCORE_LOG_WARN("walker: %s", ec.message().c_str()); ec.clear(); continue; }
    if (ShouldSkip(it->path())) {
      it.disable_recursion_pending();  // do NOT descend into skipped dir
      continue;
    }
    if (Matches(it->path())) to_modify.push_back(it->path());
  }
} catch (const std::exception &e) {
  VXCORE_LOG_WARN("walker aborted: %s", e.what());
  return;
}

// Pass 2: mutate.
for (const auto &p : to_modify) {
  std::error_code rm_ec;
  std::filesystem::remove(p, rm_ec);
  if (rm_ec) VXCORE_LOG_WARN("remove %s: %s",
                              PathToUtf8(p).c_str(), rm_ec.message().c_str());
}
```

Rules:
- Use `directory_options::skip_permission_denied` + `std::error_code` overloads — no exceptions out of the walker.
- Default iterator does NOT follow symlinks — leave the option flags alone unless you explicitly want symlink traversal (it's almost never what you want).
- Call `disable_recursion_pending()` immediately after detecting a skip-listed directory so the walker doesn't descend into it (saves work and prevents touching things like `vx_notebook/vx_sync/`).
- Wrap path strings with `PathFromUtf8(...)` at every `std::filesystem` boundary (Windows UTF-8 safety — see [UTF-8 Path Safety](#utf-8-path-safety-critical-on-windows) above).

See `GitSyncBackend::EnsureGitkeepFiles()` in `src/sync/git_sync_backend.cpp` for a real production example.
