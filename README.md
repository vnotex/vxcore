# vxcore
vxcore is a cross-platform C/C++ library providing notebook management functionality for VNote. It offers a stable C ABI for embedding in desktop (Qt), iOS (Swift), and Android (Kotlin) applications.

## Features
- **Notebook Management**: Create, open, close notebooks (bundled & raw types)
- **Unified Node Operations**: Work with files and folders using consistent `(notebook_id, path)` API
- **Folder Operations**: Create, delete, rename, move, copy folders
- **File Operations**: Create, delete, rename, move, copy files with metadata
- **Tag System**: Create tags at notebook level, apply/remove tags on files
- **Configuration**: JSON-based configuration with session persistence
- **Search**: Integration with external search tools (future)

## API Usage

### First-match content search and existing tag assignments

Content search queries may set `"firstMatchPerFile": true` with the simple
backend. Both blob and streaming APIs return only the first qualifying occurrence
in each matched file, retaining the usual line number and UTF-16 columns. Literal,
regex, case, whole-word and excluded-line rules still apply. The matchers stop
before copying a second matching line, avoiding repeated-line allocation growth.
The default remains every occurrence; non-simple backends reject the opt-in with
`VXCORE_ERR_UNSUPPORTED`. File count, source size and result transport still need
consumer bounds; the streaming API does not enforce `maxResults`.

For bundled notes, `vxcore_file_update_tags` accepts a replacement name when it is
a current notebook definition **or already assigned to that same file**. This
preserves legacy/orphan assignments without recreating definitions. Newly
introduced undefined names are rejected before replacement; omitting an orphan
removes it, and it cannot then be reintroduced unless defined. All other file
metadata and content retain their existing update behavior.

### Platform-selected mobile extension

The shared C API remains in `vxcore/vxcore.h`. Default Windows, Linux and macOS
builds retain `vxcore_context_create(config_json, &context)` and its existing
platform/portable/test-mode roots. They do not declare, compile or install the
mobile context extension.

Android and iOS targets additionally expose `vxcore/mobile/vxcore_mobile.h`:

```c
#include <vxcore/mobile/vxcore_mobile.h>

VxCoreContextHandle context = NULL;
VxCoreError error = vxcore_context_create_with_paths(
    NULL, absolute_app_data_utf8, absolute_local_data_utf8, &context);
```

CMake selects `src/mobile/context_mobile.cpp` and
`src/mobile/config_manager_mobile.cpp` and publishes `VXCORE_MOBILE_API=1` to
linked consumers. Both factories share manager initialization; the mobile factory
copies both UTF-8 paths before configuration loading and never changes global
path defaults. `config_json` remains ignored in both factories. App data holds
`vxcore.json`; local data holds `vxsession.json`, activity and notebook-local state.
The embedding app supplies persistent sandbox locations, not cache directories.

Explicit-root contexts start with no open notebooks, buffers or workspaces. They
do not read `vxsession.json`, including stale or malformed legacy session data;
the embedding app owns recovery and explicitly admits each notebook. User
`vxcore.json` configuration still loads normally and is not rewritten by this
policy. Later explicit operations/shutdown may persist session state, but a fresh
explicit-root context never restores it. Default desktop context creation retains
its existing session restoration. This prevents untracked restored buffers and
unrequested notebook recovery from crossing app-owned context lifetimes.

Both nonempty absolute roots are validated before creating directories or loading
configuration. Existing ancestors are resolved; equal/containing roots and aliases
are rejected. Invalid UTF-8, null/empty/relative or overlapping roots return
`VXCORE_ERR_INVALID_PARAM`; inaccessible/non-directory ancestors return
`VXCORE_ERR_IO`. A null output returns `VXCORE_ERR_NULL_POINTER`; otherwise output
is null on failure. Allocation and JSON errors retain their specific codes.
Partial resources are released, but created directories are not rolled back.
Consumers serialize operations and protect roots from concurrent mutation; this is
not an adversarial filesystem sandbox.

For desktop-hosted mobile bridge tests only,
`-DVXCORE_ENABLE_MOBILE_TEST_API=ON -DVXCORE_INSTALL=OFF` compiles the extension,
including host path validation. This explicit option never selects mobile save
semantics and cannot install a desktop SDK containing the test-only extension.

### Ordinary buffer writes

CMake selects exactly one `SaveBufferFileContent` implementation by **target**:

- Desktop: `src/platform/buffer_file_save_desktop.cpp` retains the original
  in-place writer from `c3319ca`, including stream-size checks and error mapping.
  Hardlink/symlink identity and writable handles that deny deletion keep their
  existing behavior. No Windows DACL modification or recovery-directory protocol
  is added. Generic `AtomicFileWriter::Commit()` retains its replacing behavior;
  the explicit `CommitNoReplace()` path is separate.
- Android/iOS: `src/mobile/buffer_file_save_mobile.cpp` uses the POSIX-only
  `MobileFileWriter`. It writes bytes directly to an exclusive sibling, flushes
  the file, checks destination identity/type/mode, preserves mode bits, renames,
  and flushes the parent directory. Readonly, linked and nonregular destinations
  are rejected without changing their old bytes before publication. Failures do
  not mark buffers clean. A directory-flush error can occur after publication;
  an IO error does not universally mean the old body remains at the path.

The mobile policy targets app-owned files. It does not claim to copy arbitrary
ownership, ACLs or extended attributes; iOS file-protection and interruption
behavior still require device verification. Encrypted providers keep their existing
persistence path. No text decoding or EOL conversion occurs in either ordinary
writer. Neither desktop host tests nor file flushing establishes power-loss safety,
unsaved-draft journaling or startup recovery.

### Prepared cohort import and metadata-only indexing

`vxcore_folder_index_children(context, notebook_id, parent_path, names_json)`
indexes an explicit batch of existing immediate children without copying,
decoding, renaming or rewriting their bytes. The JSON input is bounded to 8 MiB
and 10,000 names; core generates the new IDs and atomically publishes the parent
once. It does not recursively filter hidden or unsupported files. All input names,
types, aliases and metadata collisions are checked before publication. Failed
prepublication work removes only newly created, still-owned child metadata;
postpublication/store errors require inspecting actual state before retrying.
Read-only refusal precedes unsupported raw-notebook refusal. This is a private
construction primitive, not a replacement for the final cohort transaction.

`vxcore_folder_attach_imported` publishes already-final content/metadata and IDs
from an ordinary immediate child of this notebook's `vx_notebook/vx_import`
directory on the same filesystem. New schema-2 journals retain bounded binary
before/after parent images and directory identities. No-replace moves prevent
colliding targets from being overwritten. Parent replacement remains the commit
point, but an error does not prove rollback: recovery checks actual parent/root
identity and authoritative reachability, including a later rename or edit.

Recovery never restores old parent bytes over newer edits, removes foreign trees,
or deletes an unreadable journal to force the notebook open. Proven uncommitted
ownership permits rollback; ambiguity and incomplete rollback remain errors with
the journal retained. Proven-committed cleanup may remain safe residue without
making the notebook unopenable. Durable committed receipts do not replay content
after later deletion. Recovery repairs affected derived-store state itself.
Legacy journals are accepted only when ownership/commit is provable; uncommitted
legacy targets without recorded identity can require explicit recovery rather
than unsafe automatic deletion. Journal limits are depth 64, 100,000 IDs, 8 MiB
per parent image and 64 MiB total serialized bytes.

### Asset and attachment publication

Asset insertion now writes a complete temporary sibling and explicitly publishes
without replacement. File sources stream in bounded chunks; unique filename
selection remains, but a lost publication race fails rather than overwriting.
Write/file-flush/close failures do not publish; a parent-directory flush failure
after publication retains the complete file for reconciliation.

Attachment membership updates publish folder metadata atomically and keep caches
consistent with disk after errors. Insert retains complete unindexed bytes if
metadata publication fails. Delete stops before removing bytes when untracking
fails. Rename keeps collision suffix behavior, uses a no-replace move, performs one
membership replacement, and reports metadata errors instead of false success.
Rename/remove do not rewrite note links. Physical removal still prefers recycling
and may fall back to permanent deletion; callers must disclose that choice.

These are safer primitives, not an atomic multi-step attachment transaction.
Callers must retain intent/source and reconcile physical bytes plus membership
before retrying an ambiguous mutation. Protected note bodies are not decrypted by
these resource operations. The existing replacing writer and ordinary desktop
note-body save policy are unchanged. Windows checks file flush and WRITE_THROUGH;
POSIX directory fsync and platform-specific no-replace calls require their own
runtime evidence. No file test establishes physical power-loss safety.

An isolated ordinary Windows SDK smoke batch-indexed 10,000 Unicode Markdown
files, attached the resulting cohort, reopened with all 10,002 cohort IDs intact,
and preserved every source byte. It also reconciled an induced attachment
metadata failure and a stale legacy post-commit journal. This is API/storage
proof, not a mobile UI or performance guarantee.

Affected Windows suites (import bundle, folder manager, utilities, buffer, folder,
desktop/read-only saves and node transfer) passed. Linux exercised utilities,
import recovery, batch/folder metadata and unchanged desktop saves. Android API-28
x86_64 binaries ran on the emulator for mobile buffer/asset publication, import
recovery and filesystem primitives. Android policy refused hardlink/FIFO fixture
creation, so those Android fixture cases are not claimed; Linux exercised the
special-file guards. iOS and physical power-loss remain unverified.

### Unified Node API

The library provides unified APIs for working with both files and folders using a consistent `(notebook_id, node_path)` interface. The returned JSON includes a `"type"` field (`"file"` or `"folder"`) for automatic type identification.

```c
// Get config for any node (file or folder)
char *config_json = NULL;
VxCoreError err = vxcore_node_get_config(context, notebook_id, "path/to/node", &config_json);
if (err == VXCORE_OK) {
    // Parse JSON to check type
    // { "type": "file", "name": "...", ... } or { "type": "folder", "name": "...", ... }
    vxcore_string_free(config_json);
}

// Delete any node (automatically detects type)
vxcore_node_delete(context, notebook_id, "path/to/node");

// Rename any node
vxcore_node_rename(context, notebook_id, "old/path", "new_name");

// Move any node to different parent
vxcore_node_move(context, notebook_id, "src/path", "dest/parent");

// Copy any node with optional new name
char *copy_id = NULL;
vxcore_node_copy(context, notebook_id, "src/path", "dest/parent", "new_name", &copy_id);
vxcore_string_free(copy_id);

// Get/update metadata for any node
char *metadata_json = NULL;
vxcore_node_get_metadata(context, notebook_id, "path/to/node", &metadata_json);
vxcore_node_update_metadata(context, notebook_id, "path/to/node", "{\"key\": \"value\"}");
vxcore_string_free(metadata_json);
```

**Unified APIs:**
- `vxcore_node_get_config` - Get node config with type indicator
- `vxcore_node_delete` - Delete file or folder
- `vxcore_node_rename` - Rename file or folder
- `vxcore_node_move` - Move file or folder to different parent
- `vxcore_node_copy` - Copy file or folder with optional new name
- `vxcore_node_get_metadata` - Get metadata for file or folder
- `vxcore_node_update_metadata` - Update metadata for file or folder

**Create Operations:** For creating folders and files, use `vxcore_folder_create`, `vxcore_folder_create_path`, and `vxcore_file_create`. For file-specific tag operations, use `vxcore_file_update_tags`, `vxcore_file_tag`, and `vxcore_file_untag`.


## Build

iOS explicitly uses bundled curl and target OpenSSL; desktop Apple keeps SDK
libcurl/SecureTransport. Cross-compilers must supply the target OpenSSL paths, not
host libraries. All notebook, Git, encryption and WebDAV features remain linked.
This selection alone is not evidence of an exercised iOS build or TLS trust setup.

### Configure
```bash
cmake -B build -DVXCORE_BUILD_TESTS=ON
```

### Build
```bash
cmake --build build
```

### Run Tests
```bash
# All tests
ctest --test-dir build -C Debug  # Windows
ctest --test-dir build           # Unix

# Single module
ctest --test-dir build -C Debug -R test_core
```

## License
See LICENSE file
