#ifndef VXCORE_H
#define VXCORE_H

#include "vxcore_types.h"

#ifdef __cplusplus
extern "C" {
#endif

VXCORE_API VxCoreVersion vxcore_get_version(void);

VXCORE_API const char *vxcore_get_version_string(void);

VXCORE_API const char *vxcore_error_message(VxCoreError error);

VXCORE_API void vxcore_set_test_mode(int enabled);

VXCORE_API void vxcore_clear_test_directory(void);

VXCORE_API void vxcore_set_app_info(const char *org_name, const char *app_name);

VXCORE_API void vxcore_get_execution_file_path(char **out_path);

VXCORE_API void vxcore_get_execution_folder_path(char **out_path);

VXCORE_API VxCoreError vxcore_context_create(const char *config_json,
                                             VxCoreContextHandle *out_context);

VXCORE_API void vxcore_context_destroy(VxCoreContextHandle context);

// Snapshot current session state (buffers, workspaces) to disk.
// Sets the shutdown_called flag, preventing destructor double-saves.
// Idempotent: no-op if already called.
VXCORE_API VxCoreError vxcore_prepare_shutdown(VxCoreContextHandle context);

// Reset the shutdown_called flag so normal mutations resume.
// Safe to call in any state (no-op if not in shutdown state).
// Use when the application's close operation is cancelled by the user.
VXCORE_API VxCoreError vxcore_cancel_shutdown(VxCoreContextHandle context);

VXCORE_API VxCoreError vxcore_context_get_last_error(VxCoreContextHandle context,
                                                     const char **out_message);

VXCORE_API VxCoreError vxcore_context_get_data_path(VxCoreContextHandle context,
                                                    VxCoreDataLocation location, char **out_path);

VXCORE_API VxCoreError vxcore_context_get_config_path(VxCoreContextHandle context, char **out_path);

VXCORE_API VxCoreError vxcore_context_get_session_config_path(VxCoreContextHandle context,
                                                              char **out_path);

VXCORE_API VxCoreError vxcore_context_get_config(VxCoreContextHandle context, char **out_json);

// Update main configuration with the provided JSON object.
// Merges the provided top-level fields into the existing in-memory config
// (via JSON merge_patch over the current serialized form), then persists the
// full config to disk. Fields the caller does not include are preserved.
// Recognized top-level keys: "version" (string), "search" (object),
// "fileTypes" (object), "recoverLastSession" (boolean). Unknown keys are
// silently ignored.
// The argument must parse to a JSON object; returns VXCORE_ERR_INVALID_PARAM
// otherwise.
VXCORE_API VxCoreError vxcore_context_update_config(VxCoreContextHandle context,
                                                    const char *config_json);

VXCORE_API VxCoreError vxcore_context_get_session_config(VxCoreContextHandle context,
                                                         char **out_json);

VXCORE_API VxCoreError vxcore_context_get_config_by_name(VxCoreContextHandle context,
                                                         VxCoreDataLocation location,
                                                         const char *base_name, char **out_json);

VXCORE_API VxCoreError vxcore_context_get_config_by_name_with_defaults(VxCoreContextHandle context,
                                                                       VxCoreDataLocation location,
                                                                       const char *base_name,
                                                                       const char *default_json,
                                                                       char **out_json);

VXCORE_API VxCoreError vxcore_context_update_config_by_name(VxCoreContextHandle context,
                                                            VxCoreDataLocation location,
                                                            const char *base_name,
                                                            const char *json);

VXCORE_API VxCoreError vxcore_notebook_create(VxCoreContextHandle context, const char *path,
                                              const char *config_json, VxCoreNotebookType type,
                                              char **out_notebook_id);

VXCORE_API VxCoreError vxcore_notebook_open(VxCoreContextHandle context, const char *path,
                                            char **out_notebook_id);

VXCORE_API VxCoreError vxcore_notebook_close(VxCoreContextHandle context, const char *notebook_id);

VXCORE_API VxCoreError vxcore_notebook_list(VxCoreContextHandle context, char **out_notebooks_json);

VXCORE_API VxCoreError vxcore_notebook_get_config(VxCoreContextHandle context,
                                                  const char *notebook_id, char **out_config_json);

VXCORE_API VxCoreError vxcore_notebook_update_config(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     const char *config_json);

// Rebuilds the metadata cache for the notebook from ground truth (vx.json files).
// Use this when the cache seems out of sync or corrupted.
// The cache uses lazy sync by default - this forces a full rebuild.
VXCORE_API VxCoreError vxcore_notebook_rebuild_cache(VxCoreContextHandle context,
                                                     const char *notebook_id);

// ============ Recycle Bin Operations (Bundled Notebooks Only) ============
// Get the path to the recycle bin folder.
// Returns empty string for raw notebooks (not supported).
VXCORE_API VxCoreError vxcore_notebook_get_recycle_bin_path(VxCoreContextHandle context,
                                                            const char *notebook_id,
                                                            char **out_path);

// Empty the recycle bin by deleting all files and folders in it.
// Returns VXCORE_ERR_UNSUPPORTED for raw notebooks.
VXCORE_API VxCoreError vxcore_notebook_empty_recycle_bin(VxCoreContextHandle context,
                                                         const char *notebook_id);

// ============ Notebook History Operations ============

// Get the file opening history for a notebook.
// Returns a JSON array of history entries, ordered most-recent first.
// Each entry: {"fileId": "<uuid>", "openedUtc": <millis>}
// Returns empty array "[]" if no history exists.
// Caller must free the result with vxcore_string_free().
VXCORE_API VxCoreError vxcore_notebook_history_get(VxCoreContextHandle context,
                                                   const char *notebook_id,
                                                   char **out_history_json);

// Clear all file opening history for a notebook.
VXCORE_API VxCoreError vxcore_notebook_history_clear(VxCoreContextHandle context,
                                                      const char *notebook_id);

// Get file opening history with resolved paths for a notebook.
// Returns a JSON array of history entries with resolved relative paths, most-recent first.
// Each entry: {"fileId": "<uuid>", "openedUtc": <millis>, "relativePath": "<path>", "name": "<filename>"}
// Entries whose fileId no longer resolves (deleted files) are silently filtered out.
// Caller must free the result with vxcore_string_free().
VXCORE_API VxCoreError vxcore_notebook_history_get_resolved(VxCoreContextHandle context,
                                                             const char *notebook_id,
                                                             char **out_history_json);

// ============ Folder Operations ============
VXCORE_API VxCoreError vxcore_folder_create(VxCoreContextHandle context, const char *notebook_id,
                                            const char *parent_path, const char *folder_name,
                                            char **out_folder_id);

VXCORE_API VxCoreError vxcore_folder_create_path(VxCoreContextHandle context,
                                                 const char *notebook_id, const char *folder_path,
                                                 char **out_folder_id);

// List direct children of a folder (files and subfolders)
// folder_path: Path relative to notebook root ("" or "." for root)
// Output JSON: {"files": [...], "folders": [...]}
VXCORE_API VxCoreError vxcore_folder_list_children(VxCoreContextHandle context,
                                                   const char *notebook_id, const char *folder_path,
                                                   char **out_children_json);

// List external (unindexed) nodes in a folder.
// External nodes exist on filesystem but are not tracked in metadata.
// folder_path: Path relative to notebook root ("" or "." for root)
// Output JSON: {"files": [...], "folders": [...]}
// Each entry contains only "name" field (no ID since not indexed).
VXCORE_API VxCoreError vxcore_folder_list_external(VxCoreContextHandle context,
                                                   const char *notebook_id, const char *folder_path,
                                                   char **out_external_json);

// Get an available name for a new node (file or folder) in a folder.
// If new_name is available, out_available_name will be set to new_name.
// If new_name exists, tries new_name_1, new_name_2, etc. until an available name is found.
// For files with extensions, the suffix is inserted before the extension (e.g., file_1.txt).
// folder_path: Path relative to notebook root ("" or "." for root)
// new_name: Desired name for the new node
// out_available_name: Receives the available name (caller must free with vxcore_string_free)
VXCORE_API VxCoreError vxcore_folder_get_available_name(VxCoreContextHandle context,
                                                        const char *notebook_id,
                                                        const char *folder_path,
                                                        const char *new_name,
                                                        char **out_available_name);
// ============ File Operations ============
VXCORE_API VxCoreError vxcore_file_create(VxCoreContextHandle context, const char *notebook_id,
                                          const char *folder_path, const char *file_name,
                                          char **out_file_id);

// Import an external file into a notebook folder.
// The file is copied (not moved) to the notebook.
// If a file with the same name exists, a unique name is generated (e.g., file_1.txt).
// out_file_id: receives the ID of the imported file (caller must free with vxcore_string_free)
VXCORE_API VxCoreError vxcore_file_import(VxCoreContextHandle context, const char *notebook_id,
                                          const char *folder_path, const char *external_file_path,
                                          char **out_file_id);

// Import an external folder into a notebook folder recursively.
// The folder and all its contents are copied (not moved) to the notebook.
// All files and subfolders are indexed into the notebook's metadata system.
// If a folder with the same name exists, a unique name is generated (e.g., folder_1).
// out_folder_id: receives the ID of the imported root folder (caller must free with
// vxcore_string_free) external_folder_path: must be an absolute path to a folder outside the
// notebook root suffix_allowlist: semicolon-separated list of file extensions to import (e.g.,
// "txt;md;json"),
//                   or NULL/empty to import all files
VXCORE_API VxCoreError vxcore_folder_import(VxCoreContextHandle context, const char *notebook_id,
                                            const char *dest_folder_path,
                                            const char *external_folder_path,
                                            const char *suffix_allowlist, char **out_folder_id);

VXCORE_API VxCoreError vxcore_file_update_tags(VxCoreContextHandle context, const char *notebook_id,
                                               const char *file_path, const char *tags_json);

// Update the list of attachments for a file node (bundled notebooks).
// attachments_json: JSON array of relative filenames, e.g. ["doc.pdf", "data.zip"]
VXCORE_API VxCoreError vxcore_file_update_attachments(VxCoreContextHandle context,
                                                      const char *notebook_id,
                                                      const char *file_path,
                                                      const char *attachments_json);

// Add a single attachment to a file node (bundled notebooks).
// Idempotent: if attachment already exists in the file's list, returns
// VXCORE_OK without modifying the file's metadata or emitting an event.
// attachment: relative filename, e.g. "doc.pdf"
VXCORE_API VxCoreError vxcore_file_add_attachment(VxCoreContextHandle context,
                                                  const char *notebook_id,
                                                  const char *file_path,
                                                  const char *attachment);

// Remove a single attachment from a file node (bundled notebooks).
// Idempotent: if attachment is not present in the file's list, returns
// VXCORE_OK without modifying the file's metadata or emitting an event.
// attachment: relative filename to remove
VXCORE_API VxCoreError vxcore_file_delete_attachment(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     const char *file_path,
                                                     const char *attachment);

VXCORE_API VxCoreError vxcore_file_tag(VxCoreContextHandle context, const char *notebook_id,
                                       const char *file_path, const char *tag_name);

VXCORE_API VxCoreError vxcore_file_untag(VxCoreContextHandle context, const char *notebook_id,
                                         const char *file_path, const char *tag_name);

// Peek at the first N bytes of a file's content.
// file_path: Path relative to notebook root
// max_bytes: Maximum number of bytes to read
// out_content: receives the content (caller must free with vxcore_string_free)
// Returns VXCORE_ERR_NOT_FOUND if file doesn't exist.
VXCORE_API VxCoreError vxcore_file_peek(VxCoreContextHandle context, const char *notebook_id,
                                        const char *file_path, size_t max_bytes,
                                        char **out_content);

// ============ Node Operations (Unified File/Folder API) ============
// These APIs work with both files and folders using the same (notebook_id, node_path) pair.
// The returned JSON includes a "type" field ("file" or "folder") for caller identification.

// Get node config (unified version of folder_get_config and file_get_info)
VXCORE_API VxCoreError vxcore_node_get_config(VxCoreContextHandle context, const char *notebook_id,
                                              const char *node_path, char **out_config_json);

// Delete node (file or folder)
VXCORE_API VxCoreError vxcore_node_delete(VxCoreContextHandle context, const char *notebook_id,
                                          const char *node_path);

// Rename node (file or folder)
VXCORE_API VxCoreError vxcore_node_rename(VxCoreContextHandle context, const char *notebook_id,
                                          const char *node_path, const char *new_name);

// Move node to a different parent folder
VXCORE_API VxCoreError vxcore_node_move(VxCoreContextHandle context, const char *notebook_id,
                                        const char *src_path, const char *dest_parent_path);

// Copy node to a different parent folder with optional new name
VXCORE_API VxCoreError vxcore_node_copy(VxCoreContextHandle context, const char *notebook_id,
                                        const char *src_path, const char *dest_parent_path,
                                        const char *new_name, char **out_node_id);

// Get node metadata
VXCORE_API VxCoreError vxcore_node_get_metadata(VxCoreContextHandle context,
                                                const char *notebook_id, const char *node_path,
                                                char **out_metadata_json);

// Update node metadata
VXCORE_API VxCoreError vxcore_node_update_metadata(VxCoreContextHandle context,
                                                   const char *notebook_id, const char *node_path,
                                                   const char *metadata_json);

// Update timestamps on a node (file or folder).
// Timestamps are millisecond epoch values stored in vx.json metadata.
// If created_utc <= 0, the existing created_utc is preserved.
// If modified_utc <= 0, the existing modified_utc is preserved.
// Returns VXCORE_ERR_UNSUPPORTED for raw notebooks (no vx.json metadata).
VXCORE_API VxCoreError vxcore_node_update_timestamps(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     const char *node_path,
                                                     int64_t created_utc,
                                                     int64_t modified_utc);

// Index a filesystem node (file or folder) into the metadata store.
// The node must exist on filesystem but not be tracked in metadata.
// node_path: Path relative to notebook root (e.g., "folder/file.md")
// Returns VXCORE_ERR_NOT_FOUND if node doesn't exist on filesystem.
// Returns VXCORE_ERR_ALREADY_EXISTS if node is already indexed.
VXCORE_API VxCoreError vxcore_node_index(VxCoreContextHandle context, const char *notebook_id,
                                         const char *node_path);

// Remove a node (file or folder) from the metadata index.
// The filesystem is NOT modified - only metadata is removed.
// node_path: Path relative to notebook root (e.g., "folder/file.md")
// Returns VXCORE_ERR_NOT_FOUND if node is not in metadata.
VXCORE_API VxCoreError vxcore_node_unindex(VxCoreContextHandle context, const char *notebook_id,
                                           const char *node_path);

// Get relative path of a node (file or folder) by its ID.
// node_id: The UUID of the node
// out_path: receives the relative path (caller must free with vxcore_string_free)
// Returns VXCORE_ERR_NOT_FOUND if node is not found in metadata.
VXCORE_API VxCoreError vxcore_node_get_path_by_id(VxCoreContextHandle context,
                                                  const char *notebook_id, const char *node_id,
                                                  char **out_path);

// Resolve a node UUID to its containing notebook across all open notebooks.
// node_id: The UUID of the node (file or folder)
// out_notebook_id: receives the notebook ID (caller must free with vxcore_string_free)
// out_relative_path: receives the relative path within notebook (caller must free with
// vxcore_string_free) Returns VXCORE_ERR_NOT_FOUND if no open notebook contains a node with this
// ID.
VXCORE_API VxCoreError vxcore_node_resolve_by_id(VxCoreContextHandle context, const char *node_id,
                                                 char **out_notebook_id, char **out_relative_path);

// ============ File Type Operations ============

// Returns JSON array of all file types.
// Caller must free the returned string with vxcore_string_free().
VXCORE_API VxCoreError vxcore_filetype_list(VxCoreContextHandle context, char **out_json);

// Get file type by suffix (case-insensitive).
// Returns Others type if suffix not found.
// Caller must free the returned string with vxcore_string_free().
VXCORE_API VxCoreError vxcore_filetype_get_by_suffix(VxCoreContextHandle context,
                                                     const char *suffix, char **out_json);

// Get file type by exact name match.
// Returns VXCORE_ERR_NOT_FOUND if name not found.
// Caller must free the returned string with vxcore_string_free().
VXCORE_API VxCoreError vxcore_filetype_get_by_name(VxCoreContextHandle context, const char *name,
                                                   char **out_json);

// Set the entire file types configuration.
// Replaces all file types with the provided JSON array.
// filetype_json: JSON array of file type objects, each with:
//   - "name" (string, required, must be unique)
//   - "suffixes" (array of strings)
//   - "isNewable" (boolean)
//   - "displayName" (string)
//   - "metadata" (object, optional)
// Returns VXCORE_ERR_JSON_PARSE if JSON is invalid.
// Returns VXCORE_ERR_INVALID_PARAM if validation fails (empty name, duplicate names).
VXCORE_API VxCoreError vxcore_filetype_set(VxCoreContextHandle context, const char *filetype_json);

// ============ Tag Operations ============

VXCORE_API VxCoreError vxcore_tag_create(VxCoreContextHandle context, const char *notebook_id,
                                         const char *tag_name);

VXCORE_API VxCoreError vxcore_tag_create_path(VxCoreContextHandle context, const char *notebook_id,
                                              const char *tag_path);

VXCORE_API VxCoreError vxcore_tag_delete(VxCoreContextHandle context, const char *notebook_id,
                                         const char *tag_name);

VXCORE_API VxCoreError vxcore_tag_list(VxCoreContextHandle context, const char *notebook_id,
                                       char **out_tags_json);

VXCORE_API VxCoreError vxcore_tag_move(VxCoreContextHandle context, const char *notebook_id,
                                       const char *tag_name, const char *parent_tag);

// Find files by tags using efficient database lookup.
// tags_json: JSON array of tag name strings (e.g., ["important", "todo"])
// op: "AND" to match files with ALL tags, "OR" to match files with ANY tag
// out_results_json: receives JSON with results:
//   {"matchCount": N, "matches": [{"filePath": "...", "fileName": "...", "tags": ["...", ...]},
//   ...]}
// Caller must free out_results_json with vxcore_string_free().
VXCORE_API VxCoreError vxcore_tag_find_files(VxCoreContextHandle context, const char *notebook_id,
                                             const char *tags_json, const char *op,
                                             char **out_results_json);

// Count files per tag using efficient database lookup.
// out_results_json: receives JSON array of {tag, count} objects:
//   [{"tag": "important", "count": 5}, {"tag": "todo", "count": 3}, ...]
// Caller must free out_results_json with vxcore_string_free().
VXCORE_API VxCoreError vxcore_tag_count_files_by_tag(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     char **out_results_json);

VXCORE_API VxCoreError vxcore_search_files(VxCoreContextHandle context, const char *notebook_id,
                                           const char *query_json, const char *input_files_json,
                                           char **out_results_json);

VXCORE_API VxCoreError vxcore_search_content(VxCoreContextHandle context, const char *notebook_id,
                                             const char *query_json, const char *input_files_json,
                                             char **out_results_json);

// Extended content search with cooperative cancellation support.
// cancel_flag: Pointer to a volatile int that the caller can set to non-zero from another thread
//   to request early cancellation. If NULL, no cancellation support (behaves like
//   vxcore_search_content). Returns VXCORE_ERR_CANCELLED if cancellation was triggered.
// Caller must free out_results_json with vxcore_string_free().
VXCORE_API VxCoreError vxcore_search_content_ex(VxCoreContextHandle context,
                                                const char *notebook_id, const char *query_json,
                                                const char *input_files_json,
                                                volatile int *cancel_flag, char **out_results_json);

VXCORE_API VxCoreError vxcore_search_by_tags(VxCoreContextHandle context, const char *notebook_id,
                                             const char *query_json, const char *input_files_json,
                                             char **out_results_json);

// Resolve an absolute path to its containing notebook.
// out_notebook_id: receives the notebook ID (caller must free with vxcore_string_free)
// out_relative_path: receives the relative path within notebook (caller must free with
// vxcore_string_free) Returns VXCORE_ERR_NOT_FOUND if path is not within any open notebook.
VXCORE_API VxCoreError vxcore_path_resolve(VxCoreContextHandle context, const char *absolute_path,
                                           char **out_notebook_id, char **out_relative_path);

// Build an absolute path from a notebook ID and a relative path within that notebook.
// notebook_id: ID of an open notebook.
// relative_path: Path relative to notebook root (e.g., "folder/file.pdf").
// out_absolute_path: receives the absolute path (caller must free with vxcore_string_free).
// Returns VXCORE_ERR_NOT_FOUND if the notebook is not open.
VXCORE_API VxCoreError vxcore_path_build_absolute(VxCoreContextHandle context,
                                                  const char *notebook_id,
                                                  const char *relative_path,
                                                  char **out_absolute_path);

// ============ Workspace Operations ============

// Create a new workspace with the given name.
// Returns the workspace ID in out_id (caller must free with vxcore_string_free).
VXCORE_API VxCoreError vxcore_workspace_create(VxCoreContextHandle context, const char *name,
                                               char **out_id);

// Delete a workspace by ID.
// If this is the current workspace, the current workspace is set to another workspace.
VXCORE_API VxCoreError vxcore_workspace_delete(VxCoreContextHandle context, const char *id);

// Get workspace configuration by ID.
// Returns JSON representation (caller must free with vxcore_string_free).
VXCORE_API VxCoreError vxcore_workspace_get(VxCoreContextHandle context, const char *id,
                                            char **out_json);

// List all workspaces.
// Returns JSON array of WorkspaceRecord objects (caller must free with vxcore_string_free).
VXCORE_API VxCoreError vxcore_workspace_list(VxCoreContextHandle context, char **out_json);

// Rename a workspace.
VXCORE_API VxCoreError vxcore_workspace_rename(VxCoreContextHandle context, const char *id,
                                               const char *name);

// Get the current workspace ID.
// Returns empty string if no current workspace (caller must free with vxcore_string_free).
VXCORE_API VxCoreError vxcore_workspace_get_current(VxCoreContextHandle context, char **out_id);

// Set the current workspace by ID.
VXCORE_API VxCoreError vxcore_workspace_set_current(VxCoreContextHandle context, const char *id);

// Add a buffer to a workspace.
// Returns VXCORE_ERR_ALREADY_EXISTS if buffer is already in the workspace.
VXCORE_API VxCoreError vxcore_workspace_add_buffer(VxCoreContextHandle context,
                                                   const char *workspace_id, const char *buffer_id);

// Remove a buffer from a workspace.
VXCORE_API VxCoreError vxcore_workspace_remove_buffer(VxCoreContextHandle context,
                                                      const char *workspace_id,
                                                      const char *buffer_id);

// Set the current buffer in a workspace.
// Pass NULL or empty string to clear the current buffer.
VXCORE_API VxCoreError vxcore_workspace_set_current_buffer(VxCoreContextHandle context,
                                                           const char *workspace_id,
                                                           const char *buffer_id);

// Set the buffer order in a workspace.
// buffer_ids_json: JSON array of buffer ID strings defining the new order.
// Only buffers already in the workspace are kept; unknown IDs are ignored.
VXCORE_API VxCoreError vxcore_workspace_set_buffer_order(VxCoreContextHandle context,
                                                         const char *workspace_id,
                                                         const char *buffer_ids_json);

// Set workspace metadata (arbitrary JSON object).
// metadata_json: JSON object string to replace the workspace's metadata field.
VXCORE_API VxCoreError vxcore_workspace_set_metadata(VxCoreContextHandle context,
                                                     const char *workspace_id,
                                                     const char *metadata_json);

// Get per-buffer metadata within a workspace.
// Returns JSON object string (caller frees with vxcore_string_free).
// Returns empty object "{}" if buffer has no metadata.
VXCORE_API VxCoreError vxcore_workspace_get_buffer_metadata(VxCoreContextHandle context,
                                                            const char *workspace_id,
                                                            const char *buffer_id, char **out_json);

// Set per-buffer metadata within a workspace.
// metadata_json: JSON object string.
VXCORE_API VxCoreError vxcore_workspace_set_buffer_metadata(VxCoreContextHandle context,
                                                            const char *workspace_id,
                                                            const char *buffer_id,
                                                            const char *metadata_json);

// ============ Buffer Operations ============

// Open a file as a buffer.
// notebook_id: ID of the notebook containing the file (or NULL for external files).
// file_path: For notebook files, path relative to notebook root; for external files, absolute
// path. Returns the buffer ID in out_id (caller must free with vxcore_string_free). If the buffer
// is already open, returns the existing buffer ID.
VXCORE_API VxCoreError vxcore_buffer_open(VxCoreContextHandle context, const char *notebook_id,
                                          const char *file_path, char **out_id);

// Open a buffer by resolving a node UUID across all open notebooks.
// Combines vxcore_node_resolve_by_id + vxcore_buffer_open in a single atomic call.
// node_id: UUID of the file node to open.
// Returns the buffer ID in out_id (caller must free with vxcore_string_free).
// If the buffer is already open, returns the existing buffer ID (dedup).
// Returns VXCORE_ERR_NOT_FOUND if no open notebook contains the UUID.
VXCORE_API VxCoreError vxcore_buffer_open_by_node_id(VxCoreContextHandle context,
                                                     const char *node_id, char **out_id);

// Open a virtual buffer (no filesystem backing).
// address: Virtual address (e.g., "vx://settings"). Must not be NULL.
// Returns the buffer ID in out_id (caller must free with vxcore_string_free).
// If the buffer is already open, returns the existing buffer ID (dedup).
VXCORE_API VxCoreError vxcore_buffer_open_virtual(VxCoreContextHandle context, const char *address,
                                                  char **out_id);

// Close a buffer by ID.
VXCORE_API VxCoreError vxcore_buffer_close(VxCoreContextHandle context, const char *id);

// Get buffer configuration by ID.
// Returns JSON representation (caller must free with vxcore_string_free).
VXCORE_API VxCoreError vxcore_buffer_get(VxCoreContextHandle context, const char *id,
                                         char **out_json);

// List all open buffers.
// Returns JSON array of BufferRecord objects (caller must free with vxcore_string_free).
VXCORE_API VxCoreError vxcore_buffer_list(VxCoreContextHandle context, char **out_json);

// Save buffer content to disk.
VXCORE_API VxCoreError vxcore_buffer_save(VxCoreContextHandle context, const char *id);

// Reload buffer content from disk.
VXCORE_API VxCoreError vxcore_buffer_reload(VxCoreContextHandle context, const char *id);

// Check if a buffer's file has been modified or deleted externally.
// Updates the buffer's internal state. Query the new state with vxcore_buffer_get_state().
// Returns VXCORE_OK on success, VXCORE_ERR_BUFFER_NOT_FOUND if buffer doesn't exist.
VXCORE_API VxCoreError vxcore_buffer_check_external_changes(VxCoreContextHandle context,
                                                             const char *id);

// Get buffer content as JSON.
// Returns JSON: {"content": "<hex-encoded-bytes>"} (caller must free with vxcore_string_free).
VXCORE_API VxCoreError vxcore_buffer_get_content(VxCoreContextHandle context, const char *id,
                                                 char **out_json);

// Set buffer content from JSON.
// content_json: JSON object with "content" field containing hex-encoded bytes.
VXCORE_API VxCoreError vxcore_buffer_set_content(VxCoreContextHandle context, const char *id,
                                                 const char *content_json);

// Get buffer content as raw bytes (direct memory access for large files).
// out_data: receives pointer to internal buffer (DO NOT FREE, valid until buffer is modified or
// closed) out_size: receives size of the content in bytes
VXCORE_API VxCoreError vxcore_buffer_get_content_raw(VxCoreContextHandle context, const char *id,
                                                     const void **out_data, size_t *out_size);

// Set buffer content from raw bytes (direct memory access for large files).
// data: pointer to content bytes
// size: size of the content in bytes
VXCORE_API VxCoreError vxcore_buffer_set_content_raw(VxCoreContextHandle context, const char *id,
                                                     const void *data, size_t size);

// Get buffer state.
// out_state: receives the buffer state (NORMAL, FILE_MISSING, FILE_CHANGED, SAVE_FAILED)
VXCORE_API VxCoreError vxcore_buffer_get_state(VxCoreContextHandle context, const char *id,
                                               VxCoreBufferState *out_state);

// Check if buffer has unsaved modifications.
// out_modified: receives 1 if buffer has unsaved changes, 0 otherwise
VXCORE_API VxCoreError vxcore_buffer_is_modified(VxCoreContextHandle context, const char *id,
                                                 int *out_modified);

// Get buffer content revision number.
// Lightweight — avoids full JSON serialization of vxcore_buffer_get.
// The revision is incremented on content changes (SetContent, Save, Reload).
// out_revision: receives current revision counter.
VXCORE_API VxCoreError vxcore_buffer_get_revision(VxCoreContextHandle context, const char *id,
                                                  int *out_revision);

// ============ Buffer Backup Operations ============

// Write buffer's in-memory content to a backup file (.vswp).
// The backup file is created in the same directory as the content file.
// Returns VXCORE_ERR_INVALID_STATE if content has not been loaded.
// Returns VXCORE_ERR_IO on write failure.
VXCORE_API VxCoreError vxcore_buffer_write_backup(VxCoreContextHandle context, const char *id);

// Check if a backup file exists for this buffer.
// out_has_backup: receives 1 if backup exists, 0 otherwise.
VXCORE_API VxCoreError vxcore_buffer_has_backup(VxCoreContextHandle context, const char *id,
                                                int *out_has_backup);

// Recover buffer content from backup file, write to original file, and delete backup.
// Returns VXCORE_ERR_NOT_FOUND if no backup file exists.
VXCORE_API VxCoreError vxcore_buffer_recover_backup(VxCoreContextHandle context, const char *id);

// Discard (delete) the backup file without recovering.
// Returns VXCORE_OK even if no backup file exists (idempotent).
VXCORE_API VxCoreError vxcore_buffer_discard_backup(VxCoreContextHandle context, const char *id);

// Get the backup file path for this buffer.
// out_path: receives the backup file path. Caller must free with vxcore_string_free().
VXCORE_API VxCoreError vxcore_buffer_get_backup_path(VxCoreContextHandle context, const char *id,
                                                     char **out_path);

// ============ Buffer Asset Operations (Filesystem Only) ============
// Asset operations only touch the filesystem, they do NOT modify attachment metadata.
// Use these for embedding inline content (images, diagrams) that don't need tracking.

// Insert binary data as an asset file (NO attachment tracking).
// Creates assets folder lazily if it doesn't exist.
// If asset_name already exists, a unique name is generated (e.g., image_1.png).
//
// buffer_id: ID of an open buffer
// asset_name: Desired filename (e.g., "screenshot.png")
// data: Binary content to write
// data_size: Size of data in bytes
// out_relative_path: Receives the relative path for embedding in content
//                    (e.g., "vx_assets/<uuid>/screenshot.png" for notebook files,
//                     or "<filename>_assets/screenshot.png" for external files)
//                    Caller must free with vxcore_string_free.
// Returns VXCORE_ERR_BUFFER_NOT_FOUND if buffer doesn't exist.
// Returns VXCORE_ERR_UNSUPPORTED for raw notebooks.
// Returns VXCORE_ERR_IO on filesystem error.
VXCORE_API VxCoreError vxcore_buffer_insert_asset_raw(VxCoreContextHandle context,
                                                      const char *buffer_id, const char *asset_name,
                                                      const void *data, size_t data_size,
                                                      char **out_relative_path);

// Copy a file to assets folder (NO attachment tracking).
// Creates assets folder lazily if it doesn't exist.
// If filename already exists, a unique name is generated.
//
// source_path: Absolute path to source file
// out_relative_path: Receives the relative path for embedding in content
//                    Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_buffer_insert_asset(VxCoreContextHandle context,
                                                  const char *buffer_id, const char *source_path,
                                                  char **out_relative_path);

// Delete an asset file from the buffer's assets folder (NO attachment tracking).
// Does NOT touch attachment metadata.
//
// relative_path: Path as returned by vxcore_buffer_insert_asset*
// Returns VXCORE_ERR_NOT_FOUND if asset doesn't exist.
VXCORE_API VxCoreError vxcore_buffer_delete_asset(VxCoreContextHandle context,
                                                  const char *buffer_id, const char *relative_path);

// Get absolute path to the buffer's assets folder.
// Creates folder lazily if it doesn't exist.
//
// out_path: Receives absolute filesystem path.
//           Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_buffer_get_assets_folder(VxCoreContextHandle context,
                                                       const char *buffer_id, char **out_path);

// Get the resource base path for a buffer.
// This is the base path for resolving relative resource URLs (images, etc.) in the file's content.
// For regular files, this is the parent directory of the file.
// out_path: Receives absolute filesystem path.
//           Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_buffer_get_resource_base_path(VxCoreContextHandle context,
                                                            const char *buffer_id, char **out_path);

// ============ Buffer Attachment Operations (Filesystem + Metadata) ============
// Attachment operations modify both filesystem and attachment metadata.
// Use these for files that need to be tracked (downloadable attachments).

// Copy a file to attachments folder and add to attachment list.
// For notebook files: copies file + adds to FileRecord.attachments
// For external files: copies file only (no metadata tracking available)
//
// source_path: Absolute path to source file
// out_filename: Receives just the filename (not full path)
//               Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_buffer_insert_attachment(VxCoreContextHandle context,
                                                       const char *buffer_id,
                                                       const char *source_path,
                                                       char **out_filename);

// Delete an attachment file and remove from attachment list.
//
// filename: Just the filename (not full path)
// Returns VXCORE_ERR_NOT_FOUND if attachment doesn't exist.
VXCORE_API VxCoreError vxcore_buffer_delete_attachment(VxCoreContextHandle context,
                                                       const char *buffer_id, const char *filename);

// Rename an attachment file and update attachment list.
// If new_filename already exists, a unique name is generated.
//
// old_filename: Current filename
// new_filename: New filename
// out_new_filename: Receives the actual new filename (may differ if collision)
//                   Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_buffer_rename_attachment(VxCoreContextHandle context,
                                                       const char *buffer_id,
                                                       const char *old_filename,
                                                       const char *new_filename,
                                                       char **out_new_filename);

// List all attachments (from metadata, not filesystem scan).
// For notebook files: returns FileRecord.attachments
// For external files: returns filesystem listing (no metadata)
// Returns JSON array of filenames (not full paths).
//
// out_attachments_json: Receives JSON array (e.g., ["doc.pdf", "data.zip", ...])
//                       Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_buffer_list_attachments(VxCoreContextHandle context,
                                                      const char *buffer_id,
                                                      char **out_attachments_json);

// Get absolute path to the buffer's attachments folder.
// Creates folder lazily if it doesn't exist.
// (Same folder as assets folder - attachments and assets share location)
//
// out_path: Receives absolute filesystem path.
//           Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_buffer_get_attachments_folder(VxCoreContextHandle context,
                                                            const char *buffer_id, char **out_path);

// Get absolute path to a file node's attachments folder.
// Creates folder lazily if it doesn't exist.
// (Same folder as assets folder - attachments and assets share location)
//
// out_path: Receives absolute filesystem path.
//           Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_node_get_attachments_folder(VxCoreContextHandle context,
                                                          const char *notebook_id,
                                                          const char *file_path, char **out_path);

// List all attachments for a file node (from metadata, not filesystem scan).
// Returns JSON array of filenames (not full paths).
//
// out_attachments_json: Receives JSON array (e.g., ["doc.pdf", "data.zip", ...])
//                       Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_node_list_attachments(VxCoreContextHandle context,
                                                    const char *notebook_id, const char *file_path,
                                                    char **out_attachments_json);

/* ============ Template Operations ============ */

// Get the absolute path to the templates folder.
// Creates the folder if it doesn't exist.
// out_path: Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_template_get_folder_path(VxCoreContextHandle context,
                                                       char **out_path);

// List all templates as a JSON array of filename strings.
// Example: ["report.md", "todo.md", "notes.txt"]
// out_json: Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_template_list(VxCoreContextHandle context, char **out_json);

// List templates matching a file suffix (case-insensitive).
// suffix: Including dot, e.g. ".md", ".txt"
// out_json: JSON array of matching filenames. Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_template_list_by_suffix(VxCoreContextHandle context,
                                                      const char *suffix, char **out_json);

// Get the full content of a template file.
// name: Template filename including extension, e.g. "report.md"
// out_content: Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_template_get_content(VxCoreContextHandle context, const char *name,
                                                   char **out_content);

// Create a new template with the given name and text content.
// name: Template filename including extension, e.g. "report.md"
// content: Text content to write.
// Returns VXCORE_ERR_ALREADY_EXISTS if a template with this name exists.
// Returns VXCORE_ERR_INVALID_PARAM if name contains path separators.
VXCORE_API VxCoreError vxcore_template_create(VxCoreContextHandle context, const char *name,
                                              const char *content);

// Delete a template by name.
// Returns VXCORE_ERR_NOT_FOUND if template doesn't exist.
VXCORE_API VxCoreError vxcore_template_delete(VxCoreContextHandle context, const char *name);

// Rename a template.
// Returns VXCORE_ERR_NOT_FOUND if old_name doesn't exist.
// Returns VXCORE_ERR_ALREADY_EXISTS if new_name already exists.
// Returns VXCORE_ERR_INVALID_PARAM if new_name contains path separators.
VXCORE_API VxCoreError vxcore_template_rename(VxCoreContextHandle context, const char *old_name,
                                              const char *new_name);

/* ============ Snippet Operations ============ */

// Get the absolute path to the snippets folder.
// Creates the folder if it doesn't exist.
// out_path: Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_snippet_get_folder_path(VxCoreContextHandle context, char **out_path);

// List all snippets (built-in + user) as a JSON array of summary objects.
// Each element: {"name":"...", "type":"text"|"dynamic", "description":"...",
// "isBuiltin":true|false} out_json: Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_snippet_list(VxCoreContextHandle context, char **out_json);

// Get a single snippet by name as a JSON object with all fields.
// Fields: name, type, description, content, cursorMark, selectionMark, indentAsFirstLine, isBuiltin
// name: Snippet name (no extension, no path separators).
// out_json: Caller must free with vxcore_string_free.
// Returns VXCORE_ERR_NOT_FOUND if snippet doesn't exist.
VXCORE_API VxCoreError vxcore_snippet_get(VxCoreContextHandle context, const char *name,
                                          char **out_json);

// Create a new user snippet.
// name: Snippet name (no extension, no path separators, no '%' character).
// content_json: JSON object with snippet fields (type, description, content, cursorMark,
//               selectionMark, indentAsFirstLine).
// Returns VXCORE_ERR_ALREADY_EXISTS if name collides with built-in or existing user snippet.
// Returns VXCORE_ERR_INVALID_PARAM if name is invalid.
VXCORE_API VxCoreError vxcore_snippet_create(VxCoreContextHandle context, const char *name,
                                             const char *content_json);

// Delete a user snippet by name.
// Returns VXCORE_ERR_NOT_FOUND if snippet doesn't exist.
// Returns VXCORE_ERR_INVALID_PARAM if attempting to delete a built-in snippet.
VXCORE_API VxCoreError vxcore_snippet_delete(VxCoreContextHandle context, const char *name);

// Rename a user snippet.
// Returns VXCORE_ERR_NOT_FOUND if old_name doesn't exist.
// Returns VXCORE_ERR_ALREADY_EXISTS if new_name already exists.
// Returns VXCORE_ERR_INVALID_PARAM if renaming a built-in or new_name is invalid.
VXCORE_API VxCoreError vxcore_snippet_rename(VxCoreContextHandle context, const char *old_name,
                                             const char *new_name);

// Update a user snippet's content.
// name: Existing user snippet name.
// content_json: JSON object with updated snippet fields.
// Returns VXCORE_ERR_NOT_FOUND if snippet doesn't exist.
// Returns VXCORE_ERR_INVALID_PARAM if attempting to update a built-in snippet.
VXCORE_API VxCoreError vxcore_snippet_update(VxCoreContextHandle context, const char *name,
                                             const char *content_json);

// Apply a snippet: expand content, process cursor/selection marks, expand %name% symbols.
// name: Snippet name to apply.
// selected_text: Currently selected text (replaces selection marks). May be NULL.
// indentation: Indentation string to prepend to lines 2+. May be NULL.
// overrides_json: JSON object {"key":"value",...} for snippet overrides. May be NULL or "{}".
// out_json: Result JSON: {"text":"...","cursorOffset":N}. Caller must free with vxcore_string_free.
VXCORE_API VxCoreError vxcore_snippet_apply(VxCoreContextHandle context, const char *name,
                                            const char *selected_text, const char *indentation,
                                            const char *overrides_json, char **out_json);

// ============ Sync Operations ============

// Enable sync for a notebook.
// notebook_id: ID of the notebook.
// config_json: JSON object with sync configuration:
//   {"backend":"git","remoteUrl":"...","intervalSeconds":60}
// Default intervalSeconds is 60 (matches SyncConfig::interval_seconds in
// libs/vxcore/src/sync/sync_types.h).
// credentials_json: OPTIONAL JSON object with credentials. May be NULL.
//   When non-NULL, same shape as vxcore_sync_set_credentials (all fields
//   optional): {"pat":"...","authorName":"...","authorEmail":"..."}.
//   When NULL, the C ABI installs a NoOpCredentialProvider, which preserves
//   the legacy creds-less path: backends that declare AuthRequired will be
//   rejected by SyncManager with VXCORE_ERR_MISSING_CREDENTIALS without
//   mutating internal state, while backends that tolerate absent credentials
//   (e.g., public-read git remotes, future local backends) proceed normally.
// Returns VXCORE_ERR_UNSUPPORTED for raw notebooks.
VXCORE_API VxCoreError vxcore_sync_enable(VxCoreContextHandle context, const char *notebook_id,
                                          const char *config_json,
                                          const char *credentials_json);

// Disable sync for a notebook.
VXCORE_API VxCoreError vxcore_sync_disable(VxCoreContextHandle context, const char *notebook_id);

// Trigger an immediate sync cycle.
// Returns VXCORE_ERR_SYNC_NOT_ENABLED if sync is not enabled.
// Returns VXCORE_ERR_NOT_IMPLEMENTED if no backend is registered.
VXCORE_API VxCoreError vxcore_sync_trigger(VxCoreContextHandle context, const char *notebook_id);

// Wave 12.2 / F5.9 of sync-backend-phase4: cancellation token C ABI.
//
// Opaque handle wrapping a shared_ptr<SyncCancellation>. Lifetime is
// independent of any vxcore context — callers create a token, optionally
// pass it into vxcore_sync_trigger_cancellable, may call vxcore_sync_cancel
// from any thread at any time, and MUST eventually call
// vxcore_sync_free_cancellation to release the underlying shared_ptr.
//
// Null-safety: every function in this group is null-safe (no-ops on a
// NULL token).
typedef struct VxCoreSyncCancellation_ VxCoreSyncCancellation;

// Allocate a fresh cancellation token. Returns NULL only on allocation
// failure. The returned token is uncancelled.
VXCORE_API VxCoreSyncCancellation *vxcore_sync_create_cancellation(void);

// Request cancellation. Lock-free; safe to call from any thread, including
// concurrently with vxcore_sync_trigger_cancellable on a worker thread.
// No-op on NULL.
VXCORE_API void vxcore_sync_cancel(VxCoreSyncCancellation *token);

// Release the cancellation token. No-op on NULL. The underlying
// shared_ptr<SyncCancellation> drops one ref count — the SyncCancellation
// object itself stays alive as long as any pipeline still holds a snapshot.
VXCORE_API void vxcore_sync_free_cancellation(VxCoreSyncCancellation *token);

// Trigger an immediate sync cycle, optionally cancellable.
// When @token is NULL, behaviour is identical to vxcore_sync_trigger
// (legacy callers and back-compat). When @token is non-NULL, the token is
// installed on the backend before Sync() runs and cleared afterwards;
// concurrent vxcore_sync_cancel(@token) requests an early abort via the
// libgit2 progress callbacks.
//
// Returns VXCORE_ERR_SYNC_NOT_ENABLED if sync is not enabled.
// Returns VXCORE_ERR_NOT_IMPLEMENTED if no backend is registered.
VXCORE_API VxCoreError vxcore_sync_trigger_cancellable(VxCoreContextHandle context,
                                                       const char *notebook_id,
                                                       VxCoreSyncCancellation *token);

// vxcore-sync-stage-only V1: stage-only and network-phase entries.
//
// vxcore_sync_stage_only runs ONLY the working-tree-touching phase of a
// sync round-trip (stage index + create commit if needed). It does NOT
// contact the network and is safe to call under a per-notebook lock the
// consumer plans to release before invoking vxcore_sync_network_phase.
//
// The cancellation token, when non-NULL, is installed on the backend
// before the phase runs and cleared afterwards regardless of result
// (identical lifecycle to vxcore_sync_trigger_cancellable). Backends that
// do not support cooperative cancellation see no behavioural change.
//
// @param out_did_commit  optional; when non-NULL, set to 1 if a commit
//   object was created and 0 if there was nothing to stage. May be NULL.
//
// Returns:
//   VXCORE_OK                    success (whether or not a commit was made)
//   VXCORE_ERR_SYNC_NOT_ENABLED  sync not enabled for the notebook
//   VXCORE_ERR_NOT_IMPLEMENTED   no backend registered, or the registered
//                                backend does not override StageAndCommit
//   VXCORE_ERR_*                 propagated from the backend
VXCORE_API VxCoreError vxcore_sync_stage_only(VxCoreContextHandle context,
                                              const char *notebook_id,
                                              VxCoreSyncCancellation *token,
                                              int *out_did_commit);

// vxcore_sync_network_phase runs ONLY the network phases of a sync
// round-trip: fetch + rebase + push, including the existing retry policy.
// Callers MUST have invoked vxcore_sync_stage_only (or equivalently
// vxcore_sync_trigger_cancellable) prior so the local branch contains
// the commits to push. Safe to call WITHOUT holding the per-notebook
// lock the consumer used around vxcore_sync_stage_only.
//
// Cancellation lifecycle is identical to vxcore_sync_stage_only.
//
// Returns:
//   VXCORE_OK                    success
//   VXCORE_ERR_SYNC_NOT_ENABLED  sync not enabled for the notebook
//   VXCORE_ERR_NOT_IMPLEMENTED   no backend registered, or the registered
//                                backend does not override FetchRebasePush
//   VXCORE_ERR_SYNC_CONFLICT     rebase produced unresolved conflicts
//   VXCORE_ERR_SYNC_NETWORK      retries exhausted
VXCORE_API VxCoreError vxcore_sync_network_phase(VxCoreContextHandle context,
                                                 const char *notebook_id,
                                                 VxCoreSyncCancellation *token);

// Get sync status for a notebook.
// out_status_json: JSON output: {"state":"idle","files":[{"path":"...","status":"modified_local"}]}
// Caller must free with vxcore_string_free().
VXCORE_API VxCoreError vxcore_sync_get_status(VxCoreContextHandle context, const char *notebook_id,
                                              char **out_status_json);

// Get unresolved sync conflicts.
// out_conflicts_json: JSON output: {"conflicts":[{"path":"...","localModifiedUtc":123,"remoteModifiedUtc":456,"isBinary":false}]}
// Caller must free with vxcore_string_free().
VXCORE_API VxCoreError vxcore_sync_get_conflicts(VxCoreContextHandle context,
                                                 const char *notebook_id,
                                                 char **out_conflicts_json);

// Resolve a sync conflict.
// resolution: One of "keep_both", "keep_local", "keep_remote".
VXCORE_API VxCoreError vxcore_sync_resolve_conflict(VxCoreContextHandle context,
                                                    const char *notebook_id, const char *path,
                                                    const char *resolution);

// Set credentials for an already-enabled sync notebook (rotation path).
// credentials_json shape (all fields optional, missing = empty string):
//   {"pat":"...","authorName":"...","authorEmail":"..."}
// Wave 6.3 F4.4: internally wraps the parsed SyncCredentials in an
// InMemoryCredentialProvider and routes through
// SyncManager::UpdateCredentials. The backend reads credentials via
// ICredentialProvider exclusively — there is no SetCredentials method on
// ISyncBackend anymore. The C ABI signature is unchanged.
// Returns:
//   VXCORE_ERR_NOT_FOUND        - notebook unknown
//   VXCORE_ERR_SYNC_NOT_ENABLED - sync not enabled for notebook
//   VXCORE_ERR_NOT_IMPLEMENTED  - no concrete backend registered
VXCORE_API VxCoreError vxcore_sync_set_credentials(VxCoreContextHandle context,
                                                   const char *notebook_id,
                                                   const char *credentials_json);

// Check whether a notebook's sync configuration is complete and ready to sync.
// Returns VXCORE_OK and sets *out_ready to 1 if syncEnabled is true, syncBackend
// is non-empty, and syncRemoteUrl is non-empty. Sets *out_ready to 0 otherwise.
VXCORE_API VxCoreError vxcore_sync_is_ready(VxCoreContextHandle context, const char *notebook_id,
                                            int *out_ready);

// Check whether a notebook is REGISTERED at runtime (i.e., present in
// SyncManager's states_ map — sync was enabled successfully and the backend
// is alive). LIGHTWEIGHT metadata check: only acquires SyncManager's
// state_mutex_, never touches the backend, never calls libgit2. Safe to
// invoke from the UI thread at high frequency (sidebar repaint, sync button
// state, classify(), etc.) without racing the worker-thread sync work that
// holds the per-backend op_mutex_.
//
// Use this in preference to vxcore_sync_get_status when the caller only
// needs the "registered?" predicate. vxcore_sync_get_status additionally
// acquires the backend op_mutex_ (blocking) to enumerate per-file status,
// which races against in-flight Sync/StageAndCommit/FetchRebasePush calls
// and can starve them into VXCORE_ERR_SYNC_IN_PROGRESS.
//
// Returns VXCORE_OK on success and sets *out_registered to 1 if the
// notebook is registered, 0 otherwise. Returns VXCORE_ERR_NULL_POINTER if
// any required parameter is NULL.
VXCORE_API VxCoreError vxcore_sync_is_registered(VxCoreContextHandle context,
                                                 const char *notebook_id,
                                                 int *out_registered);

// Enable sync for a notebook with credentials supplied BEFORE backend Initialize().
// REMOVED in Wave 7.1 (sync-backend-phase4): the unified vxcore_sync_enable
// now accepts a nullable credentials_json parameter; call that instead.

// Return the per-device last successful sync timestamp for a notebook, in
// milliseconds since Unix epoch. Sets *out_utc_millis to 0 when the notebook
// has never been successfully synced on this device or when the underlying
// metadata read fails. The timestamp is persisted in metadata.db (which lives
// in the per-machine local data folder), NOT in the notebook's git-synced
// config.json, to avoid a self-sync loop.
//
// Errors:
//   VXCORE_ERR_NULL_POINTER   context, notebook_id, or out_utc_millis is null
//   VXCORE_ERR_NOT_FOUND      notebook is not loaded in NotebookManager
//   VXCORE_OK                 success (*out_utc_millis populated; may be 0)
VXCORE_API VxCoreError vxcore_sync_get_last_sync_utc(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     int64_t *out_utc_millis);

// ============ Event System ============

// Subscribe to a named event. The callback fires when the event is emitted.
// event_name: Event name string (e.g., "file.created", "notebook.opened").
// callback: Function pointer invoked with (event_name, json_data, userdata).
// userdata: Opaque pointer passed through to the callback.
// Returns VXCORE_OK on success.
VXCORE_API VxCoreError vxcore_on_event(VxCoreContextHandle context, const char *event_name,
                                       VxCoreEventCallback callback, void *userdata);

// Unsubscribe a previously registered callback from a named event.
// Removes the first matching callback function pointer.
// Returns VXCORE_ERR_NOT_FOUND if callback was not registered.
VXCORE_API VxCoreError vxcore_off_event(VxCoreContextHandle context, const char *event_name,
                                        VxCoreEventCallback callback);

// ============ Work Queue Operations ============
// Named work queues allow callers to dedicate different worker threads to
// different categories of work (e.g., "sync", "events", "indexing").
// Queues are created on first use by internal code (e.g., EventManager).
// The caller's worker thread drains a queue by calling ProcessNext in a loop.

// Process the next queued work item from the named queue, blocking up to
// timeout_ms milliseconds. Returns 1 if a work item was executed, 0 if
// timeout, shutdown, or queue does not exist.
// Pass timeout_ms <= 0 to block indefinitely until work arrives or shutdown.
VXCORE_API int vxcore_work_queue_process_next(VxCoreContextHandle context, const char *queue_name,
                                              int timeout_ms);

// Process all currently queued work items in the named queue.
// Returns the number of items processed, or 0 if queue does not exist.
VXCORE_API int vxcore_work_queue_process_all(VxCoreContextHandle context, const char *queue_name);

// Get the number of pending work items in the named queue.
// Returns 0 if queue does not exist.
VXCORE_API int vxcore_work_queue_size(VxCoreContextHandle context, const char *queue_name);

// Signal a named work queue to shut down. Unblocks any waiting ProcessNext.
// After shutdown, no new items can be enqueued to this queue. Idempotent.
// No-op if queue does not exist.
VXCORE_API void vxcore_work_queue_shutdown(VxCoreContextHandle context, const char *queue_name);

// Shut down all work queues. Idempotent.
VXCORE_API void vxcore_work_queue_shutdown_all(VxCoreContextHandle context);

VXCORE_API void vxcore_string_free(char *str);

#ifdef __cplusplus
}
#endif

#endif
