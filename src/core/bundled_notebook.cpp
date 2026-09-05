#include "bundled_notebook.h"

#include <vxcore/notebook_json_keys.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "bundled_folder_manager.h"
#include "event_manager.h"
#include "event_names.h"
#include "metadata_store.h"
#include "utils/file_utils.h"
#include "utils/logger.h"

namespace vxcore {
namespace {

namespace fs = std::filesystem;

int64_t FileTimeToUnixMillis(const fs::file_time_type &file_time) {
  const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(system_time.time_since_epoch())
          .count());
}

bool GetEntryUnixMillis(const fs::path &path, ReparseState reparse_state, int64_t *out_millis) {
  if (reparse_state == ReparseState::kNo) {
    std::error_code ec;
    const auto modified_time = fs::last_write_time(path, ec);
    if (ec) {
      return false;
    }
    *out_millis = FileTimeToUnixMillis(modified_time);
    return true;
  }

#ifdef _WIN32
  HANDLE handle = CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  FILETIME modified_time;
  const BOOL succeeded = GetFileTime(handle, nullptr, nullptr, &modified_time);
  CloseHandle(handle);
  if (!succeeded) {
    return false;
  }
  ULARGE_INTEGER ticks;
  ticks.LowPart = modified_time.dwLowDateTime;
  ticks.HighPart = modified_time.dwHighDateTime;
  constexpr uint64_t windows_epoch_offset_100ns = 116444736000000000ULL;
  if (ticks.QuadPart < windows_epoch_offset_100ns) {
    return false;
  }
  *out_millis = static_cast<int64_t>((ticks.QuadPart - windows_epoch_offset_100ns) / 10000);
  return true;
#else
  struct stat status;
  if (lstat(path.c_str(), &status) != 0) {
    return false;
  }
  *out_millis = static_cast<int64_t>(status.st_mtime) * 1000;
  return true;
#endif
}

VxCoreError RemoveRecycleBinEntry(const fs::path &path, const std::atomic_bool &cancelled,
                                  bool *out_changed, bool *out_complete) {
  *out_complete = false;
  if (cancelled.load(std::memory_order_relaxed)) {
    return VXCORE_ERR_CANCELLED;
  }

  const ReparseState reparse_state = CheckReparsePoint(PathToUtf8(path));
  if (reparse_state == ReparseState::kError) {
    VXCORE_LOG_WARN("CleanupRecycleBin: Failed to inspect %s", PathToUtf8(path).c_str());
    return VXCORE_ERR_IO;
  }

  std::error_code ec;
  if (reparse_state == ReparseState::kYes || !fs::is_directory(path, ec)) {
    if (ec) {
      VXCORE_LOG_WARN("CleanupRecycleBin: Failed to inspect entry %s: %s", PathToUtf8(path).c_str(),
                      ec.message().c_str());
      return VXCORE_ERR_IO;
    }
    const bool removed = fs::remove(path, ec);
    if (ec) {
      VXCORE_LOG_WARN("CleanupRecycleBin: Failed to remove entry %s: %s", PathToUtf8(path).c_str(),
                      ec.message().c_str());
      return VXCORE_ERR_IO;
    }
    *out_changed = *out_changed || removed;
    *out_complete = true;
    return VXCORE_OK;
  }

  VxCoreError result = VXCORE_OK;
  fs::directory_iterator iterator(path, ec);
  if (ec) {
    VXCORE_LOG_WARN("CleanupRecycleBin: Failed to enumerate %s: %s", PathToUtf8(path).c_str(),
                    ec.message().c_str());
    return VXCORE_ERR_IO;
  }

  const fs::directory_iterator end;
  while (iterator != end) {
    if (cancelled.load(std::memory_order_relaxed)) {
      return VXCORE_ERR_CANCELLED;
    }

    const fs::path child_path = iterator->path();
    iterator.increment(ec);
    if (ec) {
      VXCORE_LOG_WARN("CleanupRecycleBin: Failed to continue enumerating %s: %s",
                      PathToUtf8(path).c_str(), ec.message().c_str());
      result = VXCORE_ERR_IO;
      break;
    }

    bool child_complete = false;
    const VxCoreError child_error =
        RemoveRecycleBinEntry(child_path, cancelled, out_changed, &child_complete);
    if (child_error == VXCORE_ERR_CANCELLED) {
      return child_error;
    }
    if (child_error != VXCORE_OK) {
      result = VXCORE_ERR_IO;
    }
  }

  if (cancelled.load(std::memory_order_relaxed)) {
    return VXCORE_ERR_CANCELLED;
  }

  const bool removed = fs::remove(path, ec);
  if (ec) {
    VXCORE_LOG_WARN("CleanupRecycleBin: Failed to remove directory %s: %s",
                    PathToUtf8(path).c_str(), ec.message().c_str());
    return VXCORE_ERR_IO;
  }
  *out_changed = *out_changed || removed;
  *out_complete = true;
  return result;
}

}  // namespace

const char *BundledNotebook::kMetadataFolderName = "vx_notebook";

BundledNotebook::BundledNotebook(const std::string &local_data_folder,
                                 const std::string &root_folder)
    : Notebook(local_data_folder, root_folder, NotebookType::Bundled) {
  folder_manager_ = std::make_unique<BundledFolderManager>(this);
}

VxCoreError BundledNotebook::Create(const std::string &local_data_folder,
                                    const std::string &root_folder,
                                    const NotebookConfig *overridden_config,
                                    std::unique_ptr<Notebook> &out_notebook) {
  auto notebook =
      std::unique_ptr<BundledNotebook>(new BundledNotebook(local_data_folder, root_folder));
  if (overridden_config) {
    notebook->config_ = *overridden_config;
  }
  auto error = notebook->InitOnCreation();
  if (error != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to init bundled notebook on creation: root=%s, error=%d",
                     root_folder.c_str(), error);
    return error;
  }
  out_notebook = std::move(notebook);
  return VXCORE_OK;
}

VxCoreError BundledNotebook::InitOnCreation() {
  EnsureId();

  try {
    auto localDataPath = PathFromUtf8(GetLocalDataFolder());
    std::filesystem::create_directories(localDataPath);

    auto metadataPath = PathFromUtf8(GetMetadataFolder());
    std::filesystem::create_directories(metadataPath);
  } catch (const std::filesystem::filesystem_error &) {
    VXCORE_LOG_ERROR("Failed to create bundled notebook meta folders: root=%s",
                     root_folder_.c_str());
    return VXCORE_ERR_IO;
  }

  auto err = UpdateConfig(config_);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to save bundled notebook config: root=%s, error=%d",
                     root_folder_.c_str(), err);
    return err;
  }

  // Initialize MetadataStore
  err = InitMetadataStore();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to initialize MetadataStore: root=%s, error=%d", root_folder_.c_str(),
                     err);
    return err;
  }

  // Sync tags from NotebookConfig to MetadataStore
  err = SyncTagsToMetadataStore();
  if (err != VXCORE_OK) {
    VXCORE_LOG_WARN("Tag sync failed on creation: root=%s, error=%d", root_folder_.c_str(), err);
    // Continue anyway - tags will be synced on next open
  }

  err = folder_manager_->InitOnCreation();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to initialize bundled notebook folder manager: root=%s, error=%d",
                     root_folder_.c_str(), err);
  }
  return err;
}

VxCoreError BundledNotebook::Open(const std::string &local_data_folder,
                                  const std::string &root_folder,
                                  std::unique_ptr<Notebook> &out_notebook) {
  auto notebook =
      std::unique_ptr<BundledNotebook>(new BundledNotebook(local_data_folder, root_folder));
  auto err = notebook->LoadConfig();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to load bundled notebook config: root=%s, error=%d",
                     root_folder.c_str(), err);
    return err;
  }

  try {
    auto localDataPath = PathFromUtf8(notebook->GetLocalDataFolder());
    std::filesystem::create_directories(localDataPath);

    auto metadataPath = PathFromUtf8(notebook->GetMetadataFolder());
    std::filesystem::create_directories(metadataPath);
  } catch (const std::filesystem::filesystem_error &) {
    VXCORE_LOG_ERROR("Failed to create bundled notebook meta folders: root=%s",
                     notebook->root_folder_.c_str());
    return VXCORE_ERR_IO;
  }

  // Initialize MetadataStore
  err = notebook->InitMetadataStore();
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to initialize MetadataStore on open: root=%s, error=%d",
                     root_folder.c_str(), err);
    return err;
  }

  // Repair transfer/import journals before tag synchronization. Transfer
  // recovery may restore notebook config bytes containing tag definitions.
  if (auto *bundled_folder_manager =
          dynamic_cast<BundledFolderManager *>(notebook->GetFolderManager())) {
    int transfer_recovered = 0;
    const VxCoreError transfer_recover_err =
        bundled_folder_manager->RecoverTransfers(&transfer_recovered);
    if (transfer_recover_err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Transfer recovery failed on open: root=%s, error=%d", root_folder.c_str(),
                       transfer_recover_err);
      return transfer_recover_err;
    }
    if (transfer_recovered > 0) {
      err = notebook->LoadConfig();
      if (err != VXCORE_OK) {
        return err;
      }
      VXCORE_LOG_INFO("Recovered %d incomplete node transfer(s) on open: root=%s",
                      transfer_recovered, root_folder.c_str());
    }

    int recovered = 0;
    const VxCoreError recover_err = bundled_folder_manager->RecoverImports(&recovered);
    if (recover_err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Import recovery failed on open: root=%s, error=%d", root_folder.c_str(),
                       recover_err);
      return recover_err;
    } else if (recovered > 0) {
      VXCORE_LOG_INFO("Recovered %d incomplete folder import(s) on open: root=%s", recovered,
                      root_folder.c_str());
    }
  }

  // Sync tags from the recovered NotebookConfig to MetadataStore if needed.
  err = notebook->SyncTagsToMetadataStore();
  if (err != VXCORE_OK) {
    VXCORE_LOG_WARN("Tag sync failed on open: root=%s, error=%d", root_folder.c_str(), err);
    // Continue anyway - tags will be synced on next open or RebuildCache
  }

  // Note: We do NOT sync folder/file MetadataStore from config files here.
  // The cache uses lazy sync - data is loaded on demand when accessed.
  // Users can call RebuildCache() if they need a full refresh.

  out_notebook = std::move(notebook);
  return VXCORE_OK;
}

std::string BundledNotebook::GetMetadataFolder() const {
  return ConcatenatePaths(root_folder_, kMetadataFolderName);
}

std::string BundledNotebook::GetConfigPath() const {
  return ConcatenatePaths(GetMetadataFolder(), kConfigFileName);
}

VxCoreError BundledNotebook::LoadConfig() {
  const std::string cfg_path = GetConfigPath();
  std::ifstream file(PathFromUtf8(cfg_path));
  if (!file.is_open()) {
    const int saved_errno = errno;
    const bool exists = PathExists(cfg_path);
    const bool is_regular = IsRegularFile(cfg_path);
    VXCORE_LOG_WARN(
        "LoadConfig: open failed for cfg_path=%s exists=%d is_regular=%d errno=%d (%s) "
        "[hint: if path is under OneDrive, check 'Always keep on this device']",
        cfg_path.c_str(), exists ? 1 : 0, is_regular ? 1 : 0, saved_errno,
        std::strerror(saved_errno));
    return VXCORE_ERR_IO;
  }

  try {
    nlohmann::json json;
    file >> json;
    auto config = NotebookConfig::FromJson(json);
    if (config.id.empty()) {
      return VXCORE_ERR_INVALID_STATE;
    }
    config_ = config;
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError BundledNotebook::UpdateConfig(const NotebookConfig &config) {
  if (IsReadOnly()) {
    return VXCORE_ERR_READ_ONLY;
  }

  assert(config_.id == config.id);

  config_ = config;

  try {
    std::ofstream file(PathFromUtf8(GetConfigPath()));
    if (!file.is_open()) {
      return VXCORE_ERR_IO;
    }

    nlohmann::json json = config_.ToJson();
    file << json.dump(2);

    // Nullptr guard: event_manager_ is inherited from Notebook (T2). It is
    // nullptr during BundledNotebook::Create's InitOnCreation pass (before
    // NotebookManager wires it via SetEventManager), so calling Emit
    // unconditionally would crash on notebook creation.
    if (event_manager_ != nullptr) {
      event_manager_->Emit(events::kNotebookConfigChanged, {{kJsonKeyNotebookId, config_.id}});
    }
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError BundledNotebook::RebuildCache() {
  auto *bundled_folder_manager = dynamic_cast<BundledFolderManager *>(folder_manager_.get());
  if (!bundled_folder_manager) {
    VXCORE_LOG_ERROR("RebuildCache: folder_manager is not BundledFolderManager");
    return VXCORE_ERR_INVALID_STATE;
  }
  return bundled_folder_manager->SyncMetadataStoreFromConfigs();
}

std::string BundledNotebook::GetRecycleBinPath() const {
  auto *bundled_folder_manager = dynamic_cast<BundledFolderManager *>(folder_manager_.get());
  if (!bundled_folder_manager) {
    VXCORE_LOG_ERROR("GetRecycleBinPath: folder_manager is not BundledFolderManager");
    return "";
  }
  return bundled_folder_manager->GetRecycleBinPath();
}

VxCoreError BundledNotebook::EmptyRecycleBin() {
  if (IsReadOnly()) {
    return VXCORE_ERR_READ_ONLY;
  }

  std::string recycle_bin_path = GetRecycleBinPath();
  try {
    auto rb_path = PathFromUtf8(recycle_bin_path);
    if (!std::filesystem::exists(rb_path)) {
      return VXCORE_OK;  // Nothing to empty
    }
    for (const auto &entry : std::filesystem::directory_iterator(rb_path)) {
      std::filesystem::remove_all(entry.path());
    }
    VXCORE_LOG_INFO("EmptyRecycleBin: Cleared recycle bin at %s", recycle_bin_path.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("EmptyRecycleBin: Failed to empty recycle bin: %s", e.what());
    return VXCORE_ERR_IO;
  }
}

VxCoreError BundledNotebook::CleanupRecycleBinPath(const std::string &recycle_bin_path,
                                                   int64_t cutoff_utc_ms,
                                                   const std::atomic_bool &cancelled,
                                                   int *out_removed_count, bool *out_changed) {
  int removed_count = 0;
  bool changed = false;
  if (out_removed_count) {
    *out_removed_count = 0;
  }
  if (out_changed) {
    *out_changed = false;
  }
  if (recycle_bin_path.empty() || cutoff_utc_ms < 0) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  const fs::path recycle_bin = PathFromUtf8(recycle_bin_path);
  std::error_code ec;
  const bool exists = fs::exists(recycle_bin, ec);
  if (ec) {
    VXCORE_LOG_WARN("CleanupRecycleBin: Failed to inspect recycle bin %s: %s",
                    recycle_bin_path.c_str(), ec.message().c_str());
    return VXCORE_ERR_IO;
  }
  if (!exists) {
    return VXCORE_OK;
  }

  VxCoreError result = VXCORE_OK;
  fs::directory_iterator iterator(recycle_bin, ec);
  if (ec) {
    VXCORE_LOG_WARN("CleanupRecycleBin: Failed to enumerate recycle bin %s: %s",
                    recycle_bin_path.c_str(), ec.message().c_str());
    return VXCORE_ERR_IO;
  }

  const fs::directory_iterator end;
  while (iterator != end) {
    if (cancelled.load(std::memory_order_relaxed)) {
      result = VXCORE_ERR_CANCELLED;
      break;
    }

    const fs::path entry_path = iterator->path();
    iterator.increment(ec);
    if (ec) {
      VXCORE_LOG_WARN("CleanupRecycleBin: Failed to continue enumerating %s: %s",
                      recycle_bin_path.c_str(), ec.message().c_str());
      result = VXCORE_ERR_IO;
      break;
    }

    const ReparseState reparse_state = CheckReparsePoint(PathToUtf8(entry_path));
    int64_t modified_utc_ms = 0;
    if (reparse_state == ReparseState::kError ||
        !GetEntryUnixMillis(entry_path, reparse_state, &modified_utc_ms)) {
      VXCORE_LOG_WARN("CleanupRecycleBin: Failed to read timestamp for %s",
                      PathToUtf8(entry_path).c_str());
      result = VXCORE_ERR_IO;
      continue;
    }
    if (modified_utc_ms >= cutoff_utc_ms) {
      continue;
    }

    bool complete = false;
    const VxCoreError remove_error =
        RemoveRecycleBinEntry(entry_path, cancelled, &changed, &complete);
    if (complete) {
      ++removed_count;
    }
    if (remove_error == VXCORE_ERR_CANCELLED) {
      result = remove_error;
      break;
    }
    if (remove_error != VXCORE_OK) {
      result = VXCORE_ERR_IO;
    }
  }

  if (cancelled.load(std::memory_order_relaxed)) {
    result = VXCORE_ERR_CANCELLED;
  }
  if (out_removed_count) {
    *out_removed_count = removed_count;
  }
  if (out_changed) {
    *out_changed = changed;
  }
  return result;
}

}  // namespace vxcore
