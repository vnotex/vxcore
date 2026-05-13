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

constexpr const char *kSyncStarted = "sync.started";
constexpr const char *kSyncFinished = "sync.finished";
constexpr const char *kSyncConflict = "sync.conflict";

}  // namespace events
}  // namespace vxcore

#endif  // VXCORE_EVENT_NAMES_H
