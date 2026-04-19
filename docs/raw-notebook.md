# Raw Notebook Internals

This document describes how a **raw notebook** in vxcore manages its metadata, folders, files, and assets.

## Overview

A raw notebook treats the **filesystem as ground truth**. There is no `vx_notebook/` metadata folder, no per-folder `vx.json` config files, and no folder/file tracking via JSON configs. Instead, a raw notebook discovers its contents by reading the filesystem directly. An SQLite database in the platform-local data folder acts as a **metadata cache** -- it stores UUIDs, timestamps, and user-assigned metadata for discovered nodes, but can be rebuilt from the filesystem at any time.

### Key Differences from Bundled Notebook

| Aspect | Bundled | Raw |
|--------|---------|-----|
| Ground truth | JSON config files (`vx.json`) | Filesystem |
| Metadata folder | `<root>/vx_notebook/` | None inside notebook root |
| Notebook config location | `<root>/vx_notebook/config.json` | `notebook_metadata` table in DB |
| Folder/file tracking | Per-folder `vx.json` with file lists | Filesystem scan |
| Database role | Write-through cache of JSON configs | Metadata cache of filesystem state |
| Tags | Supported (notebook-level definitions, file-level assignments) | Not supported |
| Recycle bin | `<root>/vx_notebook/recycle_bin/` | Not supported (permanent delete) |
| External nodes | Nodes on disk but not in `vx.json` | No concept -- all nodes are discovered from filesystem |
| Attachments | Tracked in `FileRecord.attachments` | Not supported |
| Portability | Self-contained (can copy the root folder) | Not portable (metadata is external) |

## Directory Layout

```
<notebook_root>/
  <user files & folders>          # actual content -- this IS the ground truth
  (no vx_notebook/ folder)
  (no vx.json files)
  (no config.json)

<local_data>/
  notebooks/<notebook_id>/
    metadata.db                  # SQLite: metadata cache + notebook config
```

**`local_data`** resolves to `%LOCALAPPDATA%/VNote` (Windows), `~/Library/Application Support/VNote` (macOS), or `~/.local/share/VNote` (Linux). In test mode it uses a temp directory.

The notebook root contains only user files and folders. All vxcore metadata lives externally in a single file: `<local_data>/notebooks/<id>/metadata.db`.

## Notebook Config (DB-stored)

Raw notebooks do **not** use a separate `config.json` file. Instead, notebook-level configuration is stored in the `notebook_metadata` key-value table inside `metadata.db`. This eliminates the redundancy of having a config file alongside the DB in the same `<local_data>/notebooks/<id>/` directory.

### Stored Keys

| Key | Value | Example |
|-----|-------|---------|
| `name` | Display name | `"My Raw Notebook"` |
| `description` | User description | `"Optional description"` |
| `assets_folder` | Assets folder config | `"vx_assets"` |
| `metadata` | Arbitrary JSON object (as string) | `"{\"key\":\"value\"}"` |

The notebook `id` is implicit -- it is used to locate the DB file itself (`<local_data>/notebooks/<id>/metadata.db`) and is also stored in the `NotebookRecord` in `vxsession.json`.

Tags-related fields (`tags`, `tagsModifiedUtc`) are not stored since tags are not supported for raw notebooks.

### Why No Config File

Bundled notebooks need `config.json` inside the notebook root (`<root>/vx_notebook/config.json`) for portability -- the config travels with the notebook. Raw notebooks have no such requirement: their metadata is already fully external. Storing config in the DB:

- **Eliminates redundancy**: no separate file next to the DB in the same directory.
- **Single source of truth**: one file (`metadata.db`) holds everything.
- **Atomic updates**: config changes are transactional via SQLite.
- **No behavioral change**: closing already deletes `<local_data>/notebooks/<id>/` entirely, so config lifetime is the same either way.

## Metadata Database (SQLite Cache)

Located at `<local_data>/notebooks/<id>/metadata.db`. Uses the same schema as bundled notebooks (schema version 4).

| Table | Purpose in Raw Notebook |
|-------|------------------------|
| `folders` | Stores UUID, name, parent hierarchy, timestamps, and metadata for discovered folders |
| `files` | Stores UUID, name, parent folder, timestamps, and metadata for discovered files |
| `tags` | Unused (tags not supported) |
| `file_tags` | Unused (tags not supported) |
| `notebook_metadata` | Key-value store for notebook-level config and metadata (name, description, assets_folder, etc.) |
| `schema_version` | Migration tracking |

### Database as Metadata Cache

The database serves the following purposes for a raw notebook:

1. **UUID assignment**: Each folder and file discovered from the filesystem gets a stable UUID stored in the DB. This UUID persists across sessions and enables external references (e.g., linking to a file by ID).
2. **User metadata**: Arbitrary JSON metadata can be attached to files and folders via the DB, since there is no `vx.json` to store it.
3. **Timestamps**: Created/modified timestamps (UTC milliseconds) are stored in the DB. These may originate from the filesystem or be overridden by the user.
4. **Fast queries**: The DB enables efficient lookups by UUID, path, or parent without walking the filesystem.

### Sync Strategy: Filesystem Scan

Since the filesystem is ground truth, the DB must be reconciled on access:

1. **Folder listing**: When `ListFolderContents` is called, the raw folder manager scans the filesystem directory and returns the actual files and folders present. It then looks up (or creates) DB records for each discovered node to attach UUIDs and metadata.
2. **New nodes**: Files or folders that appear on the filesystem but have no DB record get a new UUID assigned and are inserted into the DB.
3. **Deleted nodes**: Files or folders that have DB records but no longer exist on the filesystem are removed from the DB during reconciliation.
4. **Metadata preservation**: When a node exists in both the filesystem and the DB, the DB metadata (user-assigned metadata, timestamps) is preserved. The `name` field is updated to match the filesystem if it changed.

### Full Rebuild

`RebuildCache()` is a no-op for raw notebooks in the current implementation (the DB can be fully reconstructed by scanning the filesystem). A full rebuild would:

1. Drop and recreate all tables.
2. Recursively scan the notebook root.
3. Assign UUIDs and insert records for every folder and file.

## Notebook Lifecycle

### Creation

`NotebookManager::CreateNotebook(root_folder, Raw, config_json)`:

1. Create `<root>/` if missing.
2. `RawNotebook::Create()`:
   - Generate UUID for `config_.id`.
   - `create_directories(<local_data>/notebooks/<id>/)`.
   - Open SQLite DB at `<local_data>/notebooks/<id>/metadata.db`.
   - Write notebook config fields (`name`, `description`, `assets_folder`, `metadata`) into the `notebook_metadata` table.
   - No folder manager initialization needed (filesystem is ground truth).
3. Save `NotebookRecord` to session config (`vxsession.json`).

### Opening

`NotebookManager::OpenNotebook(root_folder)` or `LoadOpenNotebooks()`:

1. `RawNotebook::Open(local_data_folder, root_folder, id)`:
   - Set `config_.id = id` (passed from session config -- raw notebooks need the ID to locate their DB).
   - Open SQLite DB at `<local_data>/notebooks/<id>/metadata.db`.
   - Read notebook config fields from the `notebook_metadata` table.
2. Save/update `NotebookRecord` in session config.

**Key difference from bundled**: Raw notebooks require the `id` parameter on open because all metadata is stored externally. The ID is recovered from the `NotebookRecord` in `vxsession.json`.

### Closing

`NotebookManager::CloseNotebook(notebook_id)`:

1. `Notebook::Close()`: close DB, release `MetadataStore` and `FolderManager`.
2. **Delete `<local_data>/notebooks/<id>/`** (including `metadata.db` with all config and cached metadata).
3. Remove from session config, save.

After closing, all metadata for the raw notebook is gone (the single `metadata.db` is deleted). Reopening the same root folder creates a fresh notebook with a new ID and empty DB.

## Session Persistence

`NotebookRecord` in `vxsession.json`:

```json
{
  "notebooks": [
    { "id": "a1b2c3d4-...", "rootFolder": "/path/to/notebook", "type": "raw" }
  ]
}
```

The `id` field is critical for raw notebooks -- it is the only way to locate the `metadata.db` in `<local_data>/notebooks/<id>/`. If the session config is lost, the raw notebook's metadata cannot be recovered (though the user files are unaffected).

## Folder & File Operations

### Filesystem as Ground Truth

The `RawFolderManager` reads the filesystem directly for all content discovery. There are no `vx.json` config files to manage, no folder caching, and no dual-write pattern.

### Listing Contents

When listing a folder's contents, the raw folder manager:

1. Scans the filesystem directory with `std::filesystem::directory_iterator`.
2. For each entry:
   - Skips hidden files/folders (starting with `.`).
   - Looks up the node in the DB by path.
   - If found in DB: returns the existing record (with UUID, metadata, timestamps).
   - If not found in DB: assigns a new UUID, inserts a record, and returns it.
3. Optionally reconciles: removes DB records for nodes no longer on disk.

### Create / Delete / Rename / Move / Copy

These operations work directly on the filesystem:

- **Create folder**: `std::filesystem::create_directories`, then insert a DB record with a new UUID.
- **Create file**: Create the file on disk, then insert a DB record.
- **Delete**: Remove from filesystem (permanent -- no recycle bin), then delete the DB record.
- **Rename**: `std::filesystem::rename` on disk, then update the DB record's `name`.
- **Move**: `std::filesystem::rename` to new location, then update the DB record's `folder_id`.
- **Copy**: `std::filesystem::copy` on disk, then insert a new DB record with a fresh UUID.

Every operation follows: **filesystem first, DB second**. If the filesystem operation fails, the DB is not touched.

### No External Nodes

Since all content comes from the filesystem, there is no concept of "external" (untracked) nodes. `ListExternalNodes` returns an empty result -- every file and folder on disk is a first-class citizen.

### No Index / Unindex

`IndexNode` and `UnindexNode` are not applicable. Nodes are automatically discovered from the filesystem and do not need explicit indexing or unindexing.

## Tags

Tags are **not supported** for raw notebooks. All tag-related operations (`TagFile`, `UntagFile`, `UpdateFileTags`, `FindFilesByTag`, `CreateTag`, `DeleteTag`, etc.) return `VXCORE_ERR_UNSUPPORTED`.

The rationale: tags require persistent per-file metadata that survives outside the filesystem. In a bundled notebook, tags are stored in `vx.json`. A raw notebook has no such config files, and storing tags only in the DB would make them invisible to other tools and non-portable.

## Attachments

Attachments are **not supported** for raw notebooks. All attachment-related operations (`GetFileAttachments`, `UpdateFileAttachments`, `AddFileAttachment`, `DeleteFileAttachment`) return `VXCORE_ERR_UNSUPPORTED`.

The rationale: attachment tracking requires per-file metadata in `vx.json` to associate relative paths with file UUIDs. Without per-folder configs, there is no reliable place to persist this mapping as ground truth.

## Recycle Bin

Raw notebooks do **not support** a recycle bin. `GetRecycleBinPath()` returns an empty string and `EmptyRecycleBin()` returns `VXCORE_ERR_UNSUPPORTED`. Delete operations are permanent.

## Assets Folder

The `assetsFolder` config field exists in `NotebookConfig` (default `"vx_assets"`), and the base `FolderManager` methods `GetPublicAssetsFolder()` and `GetAssetsFolder()` can resolve the path. However, since attachments are not tracked, the assets folder is not actively managed by the raw notebook. Users may manually place assets there, but vxcore does not track or index them.

## Comparison Summary

```
Bundled Notebook                    Raw Notebook
================                    ============
vx_notebook/config.json             notebook_metadata table in DB
vx_notebook/contents/.../vx.json    (none -- filesystem IS the config)
vx_notebook/recycle_bin/            (none -- permanent delete)
metadata.db = cache of vx.json      metadata.db = cache of filesystem + notebook config
Dual write: vx.json + DB            Single write: filesystem + DB
Tags: yes                           Tags: no
Attachments: yes                    Attachments: no
External nodes: yes                 External nodes: N/A
Portable: yes                       Portable: no
```

## Current Implementation Status

The raw notebook implementation is **complete**. All components described in this document are fully functional:

- **Notebook lifecycle**: Create, open, close with full config persistence in the `notebook_metadata` DB table (no `config.json` file).
- **Folder operations**: Create, delete, rename, move, copy — all filesystem-first with DB record tracking.
- **File operations**: Create, delete, rename, move, copy — same filesystem-first pattern.
- **Import operations**: Import external files and folders (with optional suffix filtering) into the notebook.
- **Metadata**: Get/update arbitrary JSON metadata on files and folders via the DB. Timestamp updates supported (`modified_utc`; `created_utc` is a DB-level limitation).
- **Iteration**: `IterateAllFiles` recursively walks the notebook via `ListFolderContents`, triggering filesystem sync at each level.
- **Filesystem sync**: `SyncFolderFromFilesystem` reconciles DB records with the actual filesystem on every folder listing — new nodes get UUIDs, stale records are removed.
- **External nodes**: `ListExternalNodes` returns empty (all nodes are first-class citizens in raw notebooks).
- **Config storage**: Notebook-level config (`name`, `description`, `assetsFolder`, `metadata`) stored in the `notebook_metadata` key-value table inside `metadata.db`.

### Intentionally Unsupported

The following operations return `VXCORE_ERR_UNSUPPORTED` by design:

| Operation | Rationale |
|-----------|-----------|
| Tags (create, delete, move, file tag/untag, update tags, find by tag) | No `vx.json` to persist per-file tag assignments |
| Attachments (list, update, add, delete) | No per-file metadata config to track attachment mappings |
| Recycle bin (empty, get path returns empty string) | No `vx_notebook/recycle_bin/` folder; deletes are permanent |
| Index/unindex nodes | All nodes are auto-discovered from filesystem; explicit indexing is not applicable |
