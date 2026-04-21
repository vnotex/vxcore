# Bundled Notebook Internals

This document describes how a **bundled notebook** in vxcore manages its metadata, folder hierarchy, and file assets.

## Overview

A bundled notebook is **self-contained**: its configuration and folder-structure metadata live inside the notebook root directory under `vx_notebook/`. This makes the notebook portable (can be copied/moved as a single directory tree). An SQLite database for fast querying lives in a platform-local data folder and is treated as a **disposable cache** -- closing the notebook deletes it, and reopening rebuilds it from the on-disk JSON configs.

## Directory Layout

```
<notebook_root>/
  vx_notebook/                   # metadata folder (kMetadataFolderName)
    config.json                  # notebook-level config (NotebookConfig)
    contents/                    # per-folder metadata mirror
      vx.json                    # root folder config (FolderConfig for ".")
      sub_a/
        vx.json                  # FolderConfig for "sub_a"
      sub_a/nested/
        vx.json                  # FolderConfig for "sub_a/nested"
    recycle_bin/                  # soft-delete destination (created on demand)
  <user files & folders>          # actual content
  vx_assets/                     # default assets folder (configurable)
    <file_uuid>/                 # per-file asset directory
      image.png                  # attachment files

<local_data>/
  notebooks/<notebook_id>/
    metadata.db                  # SQLite cache (disposable)
```

**`local_data`** resolves to `%LOCALAPPDATA%/VNote` (Windows), `~/Library/Application Support/VNote` (macOS), or `~/.local/share/VNote` (Linux). In test mode it uses a temp directory.

## Notebook Config (`config.json`)

Stored at `<root>/vx_notebook/config.json`. Serialized by `NotebookConfig::ToJson()`.

```json
{
  "id": "a1b2c3d4-...",
  "name": "My Notebook",
  "description": "Optional description",
  "assetsFolder": "vx_assets",
  "metadata": {},
  "tags": [
    { "name": "work", "parent": "", "metadata": {} },
    { "name": "urgent", "parent": "work", "metadata": {} }
  ],
  "tagsModifiedUtc": 1700000000000
}
```

| Field | C++ member | Default | Notes |
|-------|-----------|---------|-------|
| `id` | `id` | generated UUID | Assigned on creation, never changes |
| `name` | `name` | (from caller) | Display name |
| `description` | `description` | `""` | |
| `assetsFolder` | `assets_folder` | `"vx_assets"` | Relative (to file's parent), absolute, or simple name |
| `metadata` | `metadata` | `{}` | Arbitrary JSON object |
| `tags` | `tags` | `[]` | Hierarchical tag definitions (notebook-level) |
| `tagsModifiedUtc` | `tags_modified_utc` | `0` | Timestamp for tag-sync protocol |

## Folder Config (`vx.json`)

Each folder tracked by the notebook has a `vx.json` stored inside `vx_notebook/contents/<relative_path>/`. This is the **ground truth** for the folder's children.

```json
{
  "id": "f0e1d2c3-...",
  "name": "sub_a",
  "createdUtc": 1700000000000,
  "modifiedUtc": 1700000000000,
  "metadata": {},
  "files": [
    {
      "id": "b5a4c3d2-...",
      "name": "note.md",
      "createdUtc": 1700000000000,
      "modifiedUtc": 1700000000000,
      "metadata": {},
      "tags": ["work"],
      "attachments": ["image.png"]
    }
  ],
  "folders": ["nested"]
}
```

- `files` lists `FileRecord` objects for every file **directly** in this folder.
- `folders` lists **names** (not full paths) of immediate child folders. Each child has its own `vx.json`.
- `attachments` on a `FileRecord` stores paths relative to `<assets_folder>/<file_uuid>/`.

## Metadata Database (SQLite Cache)

Located at `<local_data>/notebooks/<notebook_id>/metadata.db`. Schema version 4.

| Table | Purpose |
|-------|---------|
| `folders` | Folder hierarchy (id, uuid, parent_id, name, timestamps, metadata) |
| `files` | File metadata (id, uuid, folder_id, name, timestamps, metadata, attachments) |
| `tags` | Tag definitions with hierarchy (id, name, parent_id, metadata) |
| `file_tags` | Many-to-many file-tag associations |
| `notebook_metadata` | Key-value store (e.g. `tags_synced_utc`) |
| `schema_version` | Migration tracking |

The database is a **queryable cache** of the JSON config files. It enables efficient tag queries, file lookups by UUID, and path resolution without walking the filesystem.

### Write-Through Pattern

Every mutation in `BundledFolderManager` follows a strict **dual-write** pattern:

1. Update the in-memory `FolderConfig` (and save to `vx.json`).
2. Write-through the same change to `MetadataStore` (SQLite).

The JSON files are ground truth. The DB is derived.

### Lazy Sync on Access

When a `FolderConfig` is loaded from disk for the first time (cache miss), `SyncFolderToStore()` is called. It compares `config.modified_utc` against the DB record's `modified_utc` and syncs if stale. This means the DB is populated incrementally as folders are accessed, not eagerly on open.

### Full Rebuild

`RebuildCache()` calls `SyncMetadataStoreFromConfigs()` which:

1. Drops and recreates all tables.
2. Recursively walks every `vx.json` from root.
3. Inserts all folders, files, and tags into the DB in a single transaction.

## Notebook Lifecycle

### Creation

`NotebookManager::CreateNotebook(root_folder, Bundled, config_json)`:

1. Create `<root>/` if missing.
2. `BundledNotebook::Create()`:
   - Generate UUID for `config_.id`.
   - `create_directories(<local_data>/notebooks/<id>/)`.
   - `create_directories(<root>/vx_notebook/)`.
   - Write `config.json`.
   - Open SQLite DB (`InitMetadataStore`).
   - Sync tags from config to DB (`SyncTagsToMetadataStore`).
   - `BundledFolderManager::InitOnCreation()`: create root `vx.json` for `"."` and insert root folder record into DB.
3. Save `NotebookRecord` to session config (`vxsession.json`).

### Opening

`NotebookManager::OpenNotebook(root_folder)`:

1. If already open (same root), return existing ID.
2. `BundledNotebook::Open()`:
   - Read and parse `<root>/vx_notebook/config.json`.
   - Ensure local data and metadata directories exist.
   - Open SQLite DB.
   - Sync tags if `config.tagsModifiedUtc > db.tags_synced_utc`.
   - Folder/file data is **not** eagerly synced (lazy on access).
3. Save `NotebookRecord` to session config.

On application startup, `NotebookManager` constructor calls `LoadOpenNotebooks()` which iterates `session_config.notebooks` and re-opens each one.

### Closing

`NotebookManager::CloseNotebook(notebook_id)`:

1. `Notebook::Close()`: close DB, release `MetadataStore` and `FolderManager`.
2. **Delete `<local_data>/notebooks/<id>/`** (including `metadata.db`).
3. Remove from session config, save.

The bundled `config.json` and all `vx.json` files remain on disk. Reopening the same root rebuilds the DB.

## Folder Operations

All folder/file operations go through `BundledFolderManager` which implements `FolderManager`.

### Config Caching

`BundledFolderManager` maintains an in-memory `config_cache_` (`map<string, unique_ptr<FolderConfig>>`). On first access, `vx.json` is loaded from disk, cached, and lazily synced to the DB. Subsequent reads hit the cache.

Cache is invalidated on rename, move, and delete operations.

### Create Folder

1. Verify parent exists (filesystem + config).
2. `create_directories` on filesystem.
3. Add folder name to parent's `folders` list, save parent's `vx.json`.
4. Create new `vx.json` for the folder (with generated UUID).
5. Write-through: insert `StoreFolderRecord` into DB.

### Delete Folder

1. Get folder ID from config (for DB cleanup).
2. Remove from parent's `folders` list, save parent `vx.json`.
3. Move content directory to recycle bin (`vx_notebook/recycle_bin/`); fall back to `remove_all` on failure.
4. Permanently delete the folder's `vx.json` and its metadata directory.
5. Write-through: delete from DB (cascades to child files).

### Rename / Move / Copy

- **Rename**: Renames both content directory and config directory. Updates the `FolderConfig.name` and parent's folder list.
- **Move**: Relocates content and config directories. Removes from source parent, adds to dest parent. Write-through: `store->MoveFolder(id, new_parent_id)`.
- **Copy**: `fs::copy` with recursive flag. Generates new UUIDs for the copied folder and all its files. Saves new configs and inserts into DB.

## File Operations

### Create File

1. Create empty file on filesystem.
2. Append `FileRecord` (with generated UUID, timestamps) to parent `FolderConfig.files`.
3. Save parent's `vx.json`.
4. Write-through: insert `StoreFileRecord` into DB.

### Delete File

1. Remove `FileRecord` from parent config, save `vx.json`.
2. Move file to recycle bin; fall back to `fs::remove`.
3. Write-through: delete from DB.

### Rename / Move / Copy

Same dual-write pattern. Copy generates a new UUID for the copied file.

### Import

`ImportFile` copies an external file into the notebook folder, generates a `FileRecord`, and indexes it. `ImportFolder` recursively copies (with optional suffix filtering), creates `vx.json` for every subfolder, and indexes all files.

### Index / Unindex

- `IndexNode`: adds an existing-on-disk but untracked file or folder to the metadata (creates `vx.json`, adds to parent config, writes to DB).
- `UnindexNode`: removes a node from metadata **without deleting it from disk**. Deletes the `vx.json` and DB entries.

### External Nodes

`ListExternalNodes` scans the filesystem directory and compares against the `FolderConfig` to find files/folders that exist on disk but are not tracked in metadata. It skips hidden items, the `vx_notebook` metadata folder, the configured assets folder, and legacy VNote3 directories (`vx_images`, `vx_attachments`).

## Assets (Attachments)

Each file's assets are stored in a directory derived from the notebook's `assetsFolder` config and the file's UUID:

```
<assets_folder>/<file_uuid>/
```

Resolution (`FolderManager::GetAssetsFolder`):

1. `GetPublicAssetsFolder(file_path)`: resolves `assetsFolder` relative to the file's parent directory. If `assetsFolder` is an absolute path, use it directly.
2. Append the file's UUID to get the concrete per-file assets directory.

For the default config (`"vx_assets"`) and a file at `docs/note.md` with UUID `abc123`:

```
<notebook_root>/docs/vx_assets/abc123/
```

Attachments are tracked in `FileRecord.attachments` as relative paths within the asset directory (e.g. `["image.png", "diagram.svg"]`). CRUD operations on attachments (`AddFileAttachment`, `DeleteFileAttachment`, `UpdateFileAttachments`) update both the `vx.json` and the DB, with deduplication on add.

## Recycle Bin

Bundled notebooks support a soft-delete recycle bin at `<root>/vx_notebook/recycle_bin/`. When deleting files or folders, `BundledFolderManager::MoveToRecycleBin` moves the content there instead of permanently deleting it. Name collisions are resolved by appending `_1`, `_2`, etc.

`EmptyRecycleBin()` permanently deletes all contents of the recycle bin directory.

## Tag System

Tags are defined at the notebook level in `config.json` and synced to the DB.

- **Hierarchy**: Each `TagNode` has a `name` and optional `parent` (another tag name).
- **File tagging**: `FileRecord.tags` stores a list of tag names. Tag/untag operations validate the tag exists in the notebook before applying.
- **Sync protocol**: `config.tagsModifiedUtc` is compared against `db.tags_synced_utc`. On open, if config is newer, tags are synced to DB (create/update/delete orphans) in depth-first order.
- **Deletion cascade**: Deleting a tag also deletes all child tags and removes tag references from all files.

## Session Persistence

`NotebookRecord` entries in `vxsession.json`:

```json
{
  "notebooks": [
    { "id": "a1b2c3d4-...", "rootFolder": "/path/to/notebook", "type": "bundled" }
  ]
}
```

On startup, `NotebookManager::LoadOpenNotebooks()` iterates these records and calls `BundledNotebook::Open()` for each bundled entry, restoring the previous session state.
