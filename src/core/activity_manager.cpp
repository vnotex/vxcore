#include "core/activity_manager.h"

#include <ctime>

#include <nlohmann/json.hpp>

#include "core/config_manager.h"
#include "core/event_manager.h"
#include "core/event_names.h"
#include "core/metadata_store.h"
#include "core/notebook.h"
#include "core/notebook_manager.h"
#include "db/activity_db.h"
#include "db/db_manager.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "vxcore/notebook_json_keys.h"

namespace vxcore {

namespace {
// Event payload keys for file paths. These literals match the ones emitted by
// BundledFolderManager/BufferManager; they are not part of the shared
// notebook_json_keys.h SSOT set. file.created/file.saved carry "path";
// file.moved carries "oldPath"/"newPath".
constexpr const char* kPayloadKeyPath = "path";
constexpr const char* kPayloadKeyOldPath = "oldPath";
constexpr const char* kPayloadKeyNewPath = "newPath";
}  // namespace

ActivityManager::ActivityManager(ConfigManager* config_manager, NotebookManager* notebook_manager)
    : config_manager_(config_manager), notebook_manager_(notebook_manager) {}

ActivityManager::~ActivityManager() {
  // Persist anything still pending before teardown (owner thread).
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_ && activity_db_) {
      FlushLocked();
    }
  }
  if (event_manager_) {
    for (auto id : event_listener_ids_) {
      event_manager_->Unsubscribe(id);
    }
  }
}

VxCoreError ActivityManager::Initialize() {
  if (initialized_) return VXCORE_OK;
  if (!config_manager_) return VXCORE_ERR_NULL_POINTER;

  const std::string db_path = config_manager_->GetLocalDataPath() + "/activity.db";
  db_manager_ = std::make_unique<db::DbManager>();
  if (!db_manager_->Open(db_path)) {
    VXCORE_LOG_ERROR("activity: failed to open %s", db_path.c_str());
    db_manager_.reset();
    return VXCORE_ERR_DATABASE;
  }

  activity_db_ = std::make_unique<db::ActivityDb>(db_manager_->GetHandle());
  if (!activity_db_->InitializeSchema()) {
    VXCORE_LOG_ERROR("activity: failed to initialize schema");
    activity_db_.reset();
    db_manager_.reset();
    return VXCORE_ERR_DATABASE;
  }

  initialized_ = true;
  owner_thread_ = std::this_thread::get_id();
  VXCORE_LOG_INFO("activity: initialized at %s", db_path.c_str());
  return VXCORE_OK;
}

void ActivityManager::SetEventManager(EventManager* event_manager) {
  event_manager_ = event_manager;
  if (!event_manager_) return;

  auto handler = [this](const std::string& event_name, const nlohmann::json& data) {
    std::string nb_id;
    std::string old_path;
    std::string new_path;
    if (data.contains(kJsonKeyNotebookId) && data[kJsonKeyNotebookId].is_string()) {
      nb_id = data[kJsonKeyNotebookId].get<std::string>();
    }
    // file.moved carries oldPath/newPath; file.created/file.saved carry path.
    if (data.contains(kPayloadKeyNewPath) && data[kPayloadKeyNewPath].is_string()) {
      new_path = data[kPayloadKeyNewPath].get<std::string>();
    }
    if (data.contains(kPayloadKeyOldPath) && data[kPayloadKeyOldPath].is_string()) {
      old_path = data[kPayloadKeyOldPath].get<std::string>();
    }
    if (data.contains(kPayloadKeyPath) && data[kPayloadKeyPath].is_string()) {
      new_path = data[kPayloadKeyPath].get<std::string>();
    }
    OnFileEvent(event_name, nb_id, old_path, new_path);
  };

  event_listener_ids_.push_back(event_manager_->Subscribe(events::kFileCreated, handler));
  event_listener_ids_.push_back(event_manager_->Subscribe(events::kFileSaved, handler));
  event_listener_ids_.push_back(event_manager_->Subscribe(events::kFileMoved, handler));
}

void ActivityManager::OnFileEvent(const std::string& event_name, const std::string& notebook_id,
                                  const std::string& old_path, const std::string& new_path) {
  if (!initialized_) return;

  std::lock_guard<std::mutex> lock(mutex_);
  const std::string date = TodayLocalDate();

  if (event_name == events::kFileCreated) {
    pending_daily_[date].notes_created += 1;
    if (!notebook_id.empty() && !new_path.empty()) {
      // Touch the per-file entry so the created file's last-known path is
      // captured even before any read/edit.
      pending_file_[FileKey(date, notebook_id, new_path)];
    }
  } else if (event_name == events::kFileSaved) {
    pending_daily_[date].notes_edited += 1;
    if (!notebook_id.empty() && !new_path.empty()) {
      pending_file_[FileKey(date, notebook_id, new_path)].edits += 1;
    }
  } else if (event_name == events::kFileMoved) {
    if (!notebook_id.empty() && !new_path.empty()) {
      pending_renames_.emplace_back(notebook_id, old_path, new_path);
    }
  }
}

VxCoreError ActivityManager::AddFocusTime(int64_t delta_ms) {
  if (!initialized_) return VXCORE_ERR_INVALID_STATE;
  if (delta_ms <= 0) return VXCORE_OK;
  std::lock_guard<std::mutex> lock(mutex_);
  pending_daily_[TodayLocalDate()].active_ms += delta_ms;
  return VXCORE_OK;
}

VxCoreError ActivityManager::RecordRead(const std::string& notebook_id,
                                        const std::string& rel_path) {
  if (!initialized_) return VXCORE_ERR_INVALID_STATE;
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string date = TodayLocalDate();
  pending_daily_[date].notes_read += 1;
  if (!notebook_id.empty() && !rel_path.empty()) {
    pending_file_[FileKey(date, notebook_id, rel_path)].reads += 1;
  }
  return VXCORE_OK;
}

VxCoreError ActivityManager::RecordEdit(const std::string& notebook_id,
                                        const std::string& rel_path) {
  if (!initialized_) return VXCORE_ERR_INVALID_STATE;
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string date = TodayLocalDate();
  pending_daily_[date].notes_edited += 1;
  if (!notebook_id.empty() && !rel_path.empty()) {
    pending_file_[FileKey(date, notebook_id, rel_path)].edits += 1;
  }
  return VXCORE_OK;
}

VxCoreError ActivityManager::Flush() {
  if (!initialized_ || !activity_db_) return VXCORE_ERR_INVALID_STATE;
  std::lock_guard<std::mutex> lock(mutex_);
  FlushLocked();
  return VXCORE_OK;
}

void ActivityManager::FlushLocked() {
  if (!activity_db_ || !db_manager_) return;
  if (pending_daily_.empty() && pending_file_.empty() && pending_renames_.empty()) {
    return;
  }

  db_manager_->BeginTransaction();

  for (const auto& entry : pending_daily_) {
    const std::string& date = entry.first;
    const DailyDelta& d = entry.second;
    if (d.active_ms) activity_db_->AddFocusTime(date, d.active_ms);
    if (d.notes_created) activity_db_->IncrementDaily(date, "notes_created", d.notes_created);
    if (d.notes_read) activity_db_->IncrementDaily(date, "notes_read", d.notes_read);
    if (d.notes_edited) activity_db_->IncrementDaily(date, "notes_edited", d.notes_edited);
  }

  for (const auto& entry : pending_file_) {
    const std::string& date = std::get<0>(entry.first);
    const std::string& nb = std::get<1>(entry.first);
    const std::string& path = std::get<2>(entry.first);
    std::string clean;
    const std::string file_id = ResolveFileId(nb, path, &clean);
    if (file_id.empty()) continue;  // best-effort: skip unresolved per-file row
    const std::string& display = clean.empty() ? path : clean;
    activity_db_->UpsertFileActivity(date, nb, file_id, display, entry.second.reads,
                                     entry.second.edits);
  }

  for (const auto& rename : pending_renames_) {
    const std::string& nb = std::get<0>(rename);
    const std::string& old_path = std::get<1>(rename);
    const std::string& new_path = std::get<2>(rename);
    std::string clean;
    std::string file_id = ResolveFileId(nb, new_path, &clean);
    if (file_id.empty() && !old_path.empty()) {
      file_id = ResolveFileId(nb, old_path);
    }
    if (!file_id.empty()) {
      const std::string& display = clean.empty() ? new_path : clean;
      activity_db_->UpdateFilePath(nb, file_id, display);
    }
  }

  db_manager_->CommitTransaction();

  pending_daily_.clear();
  pending_file_.clear();
  pending_renames_.clear();
}

VxCoreError ActivityManager::GetRange(const std::string& from_date, const std::string& to_date,
                                      std::string& out_json) {
  if (!initialized_ || !activity_db_) return VXCORE_ERR_INVALID_STATE;
  std::lock_guard<std::mutex> lock(mutex_);
  FlushLocked();
  out_json = activity_db_->GetRange(from_date, to_date);
  return VXCORE_OK;
}

VxCoreError ActivityManager::GetHotFiles(const std::string& from_date, const std::string& to_date,
                                         int limit, std::string& out_json) {
  if (!initialized_ || !activity_db_) return VXCORE_ERR_INVALID_STATE;
  std::lock_guard<std::mutex> lock(mutex_);
  FlushLocked();
  out_json = activity_db_->GetHotFiles(from_date, to_date, limit);
  return VXCORE_OK;
}

VxCoreError ActivityManager::GetFileHistory(const std::string& notebook_id,
                                            const std::string& file_id, std::string& out_json) {
  if (!initialized_ || !activity_db_) return VXCORE_ERR_INVALID_STATE;
  std::lock_guard<std::mutex> lock(mutex_);
  FlushLocked();
  out_json = activity_db_->GetFileHistory(notebook_id, file_id);
  return VXCORE_OK;
}

std::string ActivityManager::TodayLocalDate() {
  std::time_t t = std::time(nullptr);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
  return std::string(buf);
}

std::string ActivityManager::ResolveFileId(const std::string& notebook_id,
                                           const std::string& rel_path, std::string* out_clean) {
  if (out_clean) out_clean->clear();
  if (!notebook_manager_ || notebook_id.empty() || rel_path.empty()) return "";
  // NotebookManager / MetadataStore are not thread-safe: only resolve on the
  // owner thread. Flush() and the query methods (the only callers) run there.
  if (std::this_thread::get_id() != owner_thread_) return "";
  Notebook* notebook = notebook_manager_->GetNotebook(notebook_id);
  if (!notebook) return "";
  MetadataStore* store = notebook->GetMetadataStore();
  if (!store || !store->IsOpen()) return "";

  // Normalize to a notebook-relative path (file.saved may carry an absolute
  // path; file.created is already relative). GetCleanRelativePath handles both.
  const std::string clean = notebook->GetCleanRelativePath(rel_path);
  if (clean.empty()) return "";
  if (out_clean) *out_clean = clean;

  auto rec = store->GetFileByPath(clean);
  if (!rec.has_value()) return "";
  return rec->id;
}

}  // namespace vxcore
