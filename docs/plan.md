# VNote Refactor — Architecture & Decisions

> This document summarizes the agreed design choices for decoupling VNote's UI from its core, enabling cross‑platform embedding (desktop/iOS/Android), and introducing API + plugins.

## 0) Goals
- Decouple **core** (notebooks, notes, tags, snippets, attachments, search, events) from **UI**.
- Core runs on desktop, iOS, Android; minimal deps; stable **C ABI**.
- Multiple frontends: Qt (desktop), SwiftUI/UIKit (iOS), Kotlin/Compose (Android).
- Plugins at two layers: **Core** (logic/automation) and **UI** (Qt-side customization).
- Developer‑friendly boundary using **JSON** for now; optional fast paths later.
- File‑based notes with **DB metadata**; supports folders; safe under third‑party sync.

## 1) High-level Architecture
```
Qt UI (desktop) ── C++ wrapper ─┐
Swift/Kotlin UIs ───────────────┤── C ABI (JSON)
External tools/plugins ────────┤── (optional) Msgpack‑RPC facade (later)
                               ▼
                        VNote Core (C++ lib)
  - Domain services: Notebook/Note/Tag/Snippet/Attachment/Search
  - SQLite (WAL, FTS5) for metadata + index; content as files
  - Event bus (typed JSON notifications)
  - In‑proc Lua plugin host + external plugins via RPC (later)
  - No Qt dependency
```

## 2) API/Communication Decisions
- **Primary boundary: JSON over C ABI** (UTF‑8 strings; opaque handles; error codes).
- Keep JSON for ergonomics and stability.
- **Optional** later: add MessagePack fast‑path APIs and a Msgpack‑RPC facade for external plugins/tools if profiling shows benefit.

## 3) Plugin Strategy
- **Core plugins**: In‑process **Lua 5.4** (logic only). External plugins via RPC later.
- **Qt UI plugins**: Use **Python (PySide/PyQt)** for UI customization (panes, menus, editor modes). Provide a safe **CoreBridge** from Qt to core.
- Separation of concerns: core plugins don’t render UI; UI plugins use CoreBridge for reads/writes with capability flags.

## 4) Storage & Notebook Layout
- **Notes/content as files**, metadata in **SQLite** (WAL + FTS5).
- **Folders** are physical directories.
- Proposed structure:
```
<mynotebook_root>/
  projects/
    vx_attachments/ # containing attachment folders of notes under current folder
    vx_assets/ # containing assets folder of notes under current folder
    plan.md
  personal/
    diary/
      vx_attachments/ # containing attachment folders of notes under current folder
      vx_assets/ # containing assets folder of notes under current folder
      2025-10-30.md
  vx_notebook/ # notebook metadata
    config.json # notebook configs
    events/ # optional, for sync-safe event logs (later)
    notes/ # mirrored per-folder metadata
      projects/
        vx.json
      personal/
        vx.json
        diary/
          vx.json
  vx_attachments/ # containing attachment folders of notes under current folder
    <mynote_id>/ # attachment folder of mynote.md
      abc.pdf    # attachment files of mynote.md
  vx_assets/ # containing assets folder of notes under current folder
    <mynote_id>/ # assets folder of mynote.md
      def.png    # image referenced in mynote.md
  mynote.md # notes under notebook root

<app_local_data_folder>/
  session.json # configs specific to local instance
  vnotex.json # configs that could be synced accross devices
  vnotex.log
  notebooks/ # DB files for all notebooks
    mynotebook.db
  themes/

<app_default_data_folder>/
  vnotex.json # default configs, can be overridden by the local one
  themes/
```

## 5) Metadata Location (DB Rebuild Friendly)
- Use **mirrored meta folder**: `vx_notebook/notes/<mirror of file path/>/vx.json` per folder.
- DB could be rebuilt from metadata files.

## 8) Search & Indexing
- Call third-party searching tool (such as `ag`) for full text search.
- Abstract `ISearchBackend` to allow future engines.

## 9) Events
- Typed, JSON payload notifications: `NoteCreated`, `NoteUpdated`, `NoteMoved`, `NoteDeleted`, `TagAddedToNote`, `AttachmentAdded`, `IndexUpdated`.
- C ABI: subscribe via callback; UI marshals to main thread.

## 10) Build & Packaging
- **CMake** multi-target.
- Hide symbols; export only C API.

## 11) Versioning & Compatibility
- Semantic versioning for C ABI and (future) RPC protocol.
- `vxcore_get_version()`; feature flags; forward‑compatible JSON (ignore unknown fields).

---
**Current Defaults**
- Boundary format: **JSON**.
- Core scripting: **Lua** (logic only).
- UI scripting: **Python** (Qt).
- Notes: **file-based**; metadata in per-folder `vx.json` files and cache in SQLite.

