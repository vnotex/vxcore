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

}  // namespace events
}  // namespace vxcore

#endif  // VXCORE_EVENT_NAMES_H
