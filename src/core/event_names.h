#ifndef VXCORE_EVENT_NAMES_H
#define VXCORE_EVENT_NAMES_H

namespace vxcore {
namespace events {

constexpr const char *kFileCreated = "file.created";
constexpr const char *kFileSaved = "file.saved";
constexpr const char *kFileDeleted = "file.deleted";
constexpr const char *kFileMoved = "file.moved";
constexpr const char *kFolderCreated = "folder.created";
constexpr const char *kFolderDeleted = "folder.deleted";
constexpr const char *kNotebookOpened = "notebook.opened";
constexpr const char *kNotebookClosed = "notebook.closed";

// Sync event catalog (Wave 2.1-2.10, T7-T9):
//
// sync.started: Emitted by SyncManager::TriggerSync before backend->Sync() begins.
//   Payload: {"notebookId": "<string>"}
//
// sync.finished: Emitted by SyncManager::TriggerSync after backend->Sync() completes.
//   Payload: {"notebookId": "<string>", "result": <int>} — result is VxCoreError enum value
//
// sync.conflict: Emitted by SyncManager::TriggerSync when backend->Sync() returns
//   VXCORE_ERR_SYNC_CONFLICT. Includes conflicted file paths from backend->GetConflicts().
//   Payload: {"notebookId": "<string>", "files": ["<rel-path>", ...]}
//   (rel-path is relative to notebook root; array may be populated by T8)
//
// sync.should_run: Emitted by SyncManager::MaybeEnqueueSync when dirty-tracker
//   debounce elapses for a sync-enabled notebook. Intended to be consumed by Qt's
//   EventBridge, which routes to SyncWorkQueueManager for FIFO dispatch.
//   Payload: {"notebookId": "<string>"}
//
constexpr const char *kSyncStarted = "sync.started";
constexpr const char *kSyncFinished = "sync.finished";
constexpr const char *kSyncConflict = "sync.conflict";
constexpr const char *kSyncShouldRun = "sync.should_run";

// Persistence-layer events (Wave 2-3, T4 + T8): fire on every successful
// write to the corresponding config file. SyncManager::mark_dirty subscribes
// to BOTH so any persistence write triggers auto-sync wakeup, regardless of
// which higher-level mutation produced it.
//
// folder.config_changed: emitted by BundledFolderManager::SaveFolderConfig
//   (bundled_folder_manager.cpp:256) after a successful vx.json write. ANY
//   caller of SaveFolderConfig fires this, including structural ops
//   (rename/move/copy/import) that have no dedicated event of their own.
//   Payload: {"notebookId": "<id>", "path": "<rel-folder-path>"}
//
// notebook.config_changed: emitted by BundledNotebook::UpdateConfig
//   (bundled_notebook.cpp:193-196) after a successful vx_notebook/config.json
//   write. Guarded by a nullptr check on event_manager_ (null during
//   NotebookManager::CreateNotebook's initial UpdateConfig; see T2's plumbing
//   in notebook_manager.cpp:125).
//   Payload: {"notebookId": "<id>"}
//
constexpr const char *kFolderConfigChanged = "folder.config_changed";
constexpr const char *kNotebookConfigChanged = "notebook.config_changed";

// Semantic-layer events (Wave 2-3, T5-T8): fire from public mutation methods
// with rich payloads for UI / consumer-side dispatch. NOT subscribed by
// SyncManager::mark_dirty; these are intended for UI consumers and future
// Qt EventBridge integration. The folder.config_changed event fired by the
// same call drives the auto-sync wakeup half of the contract.
//
// file.tagged: emitted by BundledFolderManager::TagFile
//   (bundled_folder_manager.cpp:1231) after a single tag is appended and
//   SaveFolderConfig succeeds.
//   Payload: {"notebookId": "<id>", "path": "<rel-file-path>", "tag": "<name>"}
//
// file.untagged: emitted by BundledFolderManager::UntagFile
//   (bundled_folder_manager.cpp:1274) after a single tag is removed and
//   SaveFolderConfig succeeds.
//   Payload: {"notebookId": "<id>", "path": "<rel-file-path>", "tag": "<name>"}
//
// file.tags_replaced: emitted by BundledFolderManager::UpdateFileTags
//   (bundled_folder_manager.cpp:1179) after a full tag-array replacement and
//   SaveFolderConfig succeeds.
//   Payload: {"notebookId": "<id>", "path": "<rel-file-path>",
//             "tags": ["<tag1>", ...]}
//
// file.metadata_updated: emitted by BundledFolderManager::UpdateFileMetadata
//   (bundled_folder_manager.cpp:1124) after a metadata-only file update.
//   Payload: {"notebookId": "<id>", "path": "<rel-file-path>"}
//
// folder.metadata_updated: emitted by BundledFolderManager::UpdateFolderMetadata
//   (bundled_folder_manager.cpp:483) after a metadata-only folder update.
//   Payload: {"notebookId": "<id>", "path": "<rel-folder-path>"}
//
// file.attached: emitted by BundledFolderManager::AddFileAttachment
//   (bundled_folder_manager.cpp:2777) after a single attachment is appended
//   and SaveFolderConfig succeeds. Idempotent on duplicate: no emit if the
//   attachment was already present.
//   Payload: {"notebookId": "<id>", "path": "<rel-file-path>",
//             "attachment": "<name>"}
//
// file.detached: emitted by BundledFolderManager::DeleteFileAttachment
//   (bundled_folder_manager.cpp:2823) after a single attachment is removed
//   and SaveFolderConfig succeeds. Idempotent on missing: no emit if the
//   attachment was already absent.
//   Payload: {"notebookId": "<id>", "path": "<rel-file-path>",
//             "attachment": "<name>"}
//
// file.attachments_replaced: emitted by
//   BundledFolderManager::UpdateFileAttachments
//   (bundled_folder_manager.cpp:2728) after a full attachment-array
//   replacement and SaveFolderConfig succeeds.
//   Payload: {"notebookId": "<id>", "path": "<rel-file-path>",
//             "attachments": ["<name1>", ...]}
//
constexpr const char *kFileTagged = "file.tagged";
constexpr const char *kFileUntagged = "file.untagged";
constexpr const char *kFileTagsReplaced = "file.tags_replaced";
constexpr const char *kFileMetadataUpdated = "file.metadata_updated";
constexpr const char *kFolderMetadataUpdated = "folder.metadata_updated";
constexpr const char *kFileAttached = "file.attached";
constexpr const char *kFileDetached = "file.detached";
constexpr const char *kFileAttachmentsReplaced = "file.attachments_replaced";

// folder.children_reordered: emitted by BundledFolderManager::SetChildrenOrder
//   after the folder's vx.json has been atomically rewritten with the new
//   child ordering. Fires on the success path ONLY — NOT on no-op (current
//   order unchanged) and NOT on error paths (permutation mismatch, parse
//   failure, missing folder). The umbrella folder.config_changed event still
//   fires from SaveFolderConfig for auto-sync wakeup; this dedicated
//   structural event lets richer UI consumers react to reorder specifically.
//   Payload: {"notebookId": "<id>", "path": "<rel-folder-path>",
//             "folders": ["<name1>", ...]?, "files": ["<name1>", ...]?}
//   The folders/files arrays are present only when the corresponding
//   sub-array was reordered in this call.
constexpr const char *kFolderChildrenReordered = "folder.children_reordered";

}  // namespace events
}  // namespace vxcore

#endif  // VXCORE_EVENT_NAMES_H
