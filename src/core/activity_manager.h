#ifndef VXCORE_ACTIVITY_MANAGER_H
#define VXCORE_ACTIVITY_MANAGER_H

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "vxcore/vxcore_types.h"

namespace vxcore {

class ConfigManager;
class NotebookManager;
class EventManager;

namespace db {
class DbManager;
class ActivityDb;
}  // namespace db

// App-global activity/statistics collector.
//
// Owns a standalone SQLite database (activity.db) under LocalData -- per-device,
// NOT the per-notebook metadata.db and NOT synced. Subscribes to EventManager
// file.created / file.saved / file.moved to record counts, and exposes
// record/query methods used by the C API.
//
// Write model: recording is CHEAP and IN-MEMORY -- record* / OnFileEvent only
// accumulate deltas into pending maps under a mutex, doing NO disk I/O and NO
// NotebookManager/MetadataStore access. The accumulated deltas are persisted in
// one batched transaction by Flush(), which the consumer drives periodically
// (~60s) and on shutdown. This keeps per-event cost off the UI/worker hot paths
// (no fsync per save/open) and defers path->file_id resolution to Flush(), which
// runs on the owner thread where NotebookManager/MetadataStore are safe to use.
// A crash loses at most one flush interval of activity.
//
// Thread-safety: recording may be called from any thread (e.g. VNote's
// BufferSaveQueue worker emits file.saved). All state is guarded by mutex_.
// Flush() and the query methods resolve file ids via NotebookManager and MUST
// run on the owner thread (the thread that called Initialize()); the C API
// invokes them on the main thread.
class ActivityManager {
 public:
  ActivityManager(ConfigManager* config_manager, NotebookManager* notebook_manager);
  ~ActivityManager();

  ActivityManager(const ActivityManager&) = delete;
  ActivityManager& operator=(const ActivityManager&) = delete;

  // Opens activity.db and initializes/migrates the schema. Captures the calling
  // thread as the owner thread. Returns VXCORE_OK on success.
  VxCoreError Initialize();

  // Subscribes to file.* events. event_manager must outlive this manager.
  void SetEventManager(EventManager* event_manager);

  // --- Recording (cheap, in-memory; any thread) ---

  // Adds focused milliseconds to today's global bucket.
  VxCoreError AddFocusTime(int64_t delta_ms);

  // Records a read for today: +1 global notes_read and a per-file read.
  VxCoreError RecordRead(const std::string& notebook_id, const std::string& rel_path);

  // Records an edit for today: +1 global notes_edited and a per-file edit.
  VxCoreError RecordEdit(const std::string& notebook_id, const std::string& rel_path);

  // Persists all pending in-memory deltas to activity.db in one transaction,
  // resolving per-file paths to stable file ids. Owner-thread only. No-op when
  // nothing is pending.
  VxCoreError Flush();

  // --- Queries (return JSON via out param; flush pending first) ---
  VxCoreError GetRange(const std::string& from_date, const std::string& to_date,
                       std::string& out_json);
  VxCoreError GetHotFiles(const std::string& from_date, const std::string& to_date, int limit,
                          std::string& out_json);
  VxCoreError GetFileHistory(const std::string& notebook_id, const std::string& file_id,
                             std::string& out_json);

 private:
  // Local calendar date 'YYYY-MM-DD' for "now" (device timezone).
  static std::string TodayLocalDate();

  // Resolves {notebook_id, path} to a stable file UUID via the owning
  // notebook's metadata store. Returns empty string if unresolved. When
  // out_clean is non-null it receives the notebook-relative cleaned path.
  //
  // NotebookManager / MetadataStore are NOT thread-safe, so this only resolves
  // on the owner thread; off-thread calls return "" without touching them.
  std::string ResolveFileId(const std::string& notebook_id, const std::string& rel_path,
                            std::string* out_clean = nullptr);

  // Shared handler for created/saved/moved events (accumulates in memory).
  void OnFileEvent(const std::string& event_name, const std::string& notebook_id,
                   const std::string& old_path, const std::string& new_path);

  // Persists pending deltas. Caller MUST hold mutex_. Owner-thread only.
  void FlushLocked();

  // Aggregated per-day global counters.
  struct DailyDelta {
    int64_t active_ms = 0;
    int64_t notes_created = 0;
    int64_t notes_read = 0;
    int64_t notes_edited = 0;
  };
  // Aggregated per-file counters keyed by (date, notebook_id, path).
  struct FileDelta {
    int64_t reads = 0;
    int64_t edits = 0;
  };
  using FileKey = std::tuple<std::string, std::string, std::string>;  // date, notebook_id, path

  ConfigManager* config_manager_ = nullptr;
  NotebookManager* notebook_manager_ = nullptr;
  EventManager* event_manager_ = nullptr;

  std::unique_ptr<db::DbManager> db_manager_;
  std::unique_ptr<db::ActivityDb> activity_db_;
  std::vector<uint64_t> event_listener_ids_;
  bool initialized_ = false;

  // Guards all pending maps AND activity.db access.
  std::mutex mutex_;

  std::map<std::string, DailyDelta> pending_daily_;
  std::map<FileKey, FileDelta> pending_file_;
  // Pending path renames: (notebook_id, old_path, new_path). Applied at flush.
  std::vector<std::tuple<std::string, std::string, std::string>> pending_renames_;

  // Thread that called Initialize(); the only thread on which NotebookManager /
  // MetadataStore may be safely accessed. Captured once, read-only thereafter.
  std::thread::id owner_thread_;
};

}  // namespace vxcore

#endif  // VXCORE_ACTIVITY_MANAGER_H
