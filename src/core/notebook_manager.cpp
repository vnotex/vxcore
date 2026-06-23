#include "notebook_manager.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>

#include <vxcore/notebook_json_keys.h>

#include "bundled_notebook.h"
#include "config_manager.h"
#include "core/event_manager.h"
#include "core/event_names.h"
#include "core/folder_manager.h"
#include "metadata_store.h"
#include "raw_notebook.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

NotebookManager::NotebookManager(ConfigManager *config_manager) : config_manager_(config_manager) {
  LoadOpenNotebooks();
}

NotebookManager::~NotebookManager() {}

void NotebookManager::LoadOpenNotebooks() {
  auto &session_config = config_manager_->GetSessionConfig();
  const auto local_data_folder = config_manager_->GetLocalDataPath();
  notebooks_.clear();

  // Deterministic reconcile/dedupe: build a fresh record list where exactly one
  // record survives per cleaned root, a loadable record always wins over a
  // phantom, and a stale persisted id is healed against the loaded config.json
  // id. Phantoms (failed-to-load, e.g. folder not yet hydrated by OneDrive) are
  // preserved so a notebook is never silently lost.
  std::vector<NotebookRecord> reconciled;
  reconciled.reserve(session_config.notebooks.size());
  std::unordered_map<std::string, size_t> root_index;   // cleaned root -> index in reconciled
  std::unordered_map<std::string, bool> root_loaded;     // cleaned root -> slot is loaded vs phantom
  bool changed = false;

  for (const auto &record : session_config.notebooks) {
    const std::string root_clean = CleanPath(record.root_folder);

    std::unique_ptr<Notebook> notebook;
    switch (record.type) {
      case NotebookType::Bundled: {
        auto error = BundledNotebook::Open(local_data_folder, root_clean, notebook);
        if (error != VXCORE_OK) {
          VXCORE_LOG_ERROR("Failed to load bundled notebook: root_folder=%s, error=%d",
                           record.root_folder.c_str(), error);
        }
        break;
      }
      case NotebookType::Raw: {
        auto error = RawNotebook::Open(local_data_folder, root_clean, record.id, notebook);
        if (error != VXCORE_OK) {
          VXCORE_LOG_ERROR("Failed to load raw notebook: root_folder=%s, error=%d",
                           record.root_folder.c_str(), error);
        }
        break;
      }
      default:
        VXCORE_LOG_ERROR("Skip invalid notebook type: %d", static_cast<int>(record.type));
        break;
    }

    if (notebook) {
      // T15: re-apply the persisted per-device read-only flag before the
      // notebook is published to the manager's map, so a downstream
      // GetNotebook(id)->IsReadOnly() sees the same state the session was
      // shut down in. Missing field is back-compat (defaults to false).
      notebook->SetReadOnly(record.read_only);

      // Reconcile a stale persisted id against the ground-truth id loaded from
      // config.json (the root_folder is the stable identity on this device).
      NotebookRecord kept = record;
      if (record.id != notebook->GetId()) {
        VXCORE_LOG_WARN("LoadOpenNotebooks: reconciling stale record id %s -> %s for root=%s",
                        record.id.c_str(), notebook->GetId().c_str(), root_clean.c_str());
        kept.id = notebook->GetId();
        changed = true;
      }

      VXCORE_LOG_INFO("Loaded open notebook: id=%s, root_folder=%s, read_only=%d",
                      notebook->GetId().c_str(), notebook->GetRootFolder().c_str(),
                      record.read_only ? 1 : 0);

      if (root_clean.empty()) {
        // Malformed empty root: always keep, never dedupe/overwrite.
        reconciled.push_back(std::move(kept));
      } else {
        auto found = root_index.find(root_clean);
        if (found == root_index.end()) {
          // Root unseen: append and remember its slot as loaded.
          root_index[root_clean] = reconciled.size();
          root_loaded[root_clean] = true;
          reconciled.push_back(std::move(kept));
        } else if (!root_loaded[root_clean]) {
          // Existing slot is a phantom: a loadable record replaces it.
          reconciled[found->second] = std::move(kept);
          root_loaded[root_clean] = true;
          changed = true;
        } else {
          // Slot already loaded: drop this duplicate.
          changed = true;
        }
      }

      notebooks_[notebook->GetId()] = std::move(notebook);
    } else {
      // Phantom: KEEP the record (a load failure is often transient) but dedupe
      // duplicate phantoms / phantoms shadowed by a same-root loaded record.
      if (root_clean.empty()) {
        // Malformed empty root: always keep, never dedupe.
        VXCORE_LOG_WARN("LoadOpenNotebooks: keeping unloaded (phantom) record id=%s root=%s",
                        record.id.c_str(), root_clean.c_str());
        reconciled.push_back(record);
      } else if (root_index.find(root_clean) == root_index.end()) {
        // Root unseen: keep the phantom and remember its slot.
        VXCORE_LOG_WARN("LoadOpenNotebooks: keeping unloaded (phantom) record id=%s root=%s",
                        record.id.c_str(), root_clean.c_str());
        root_index[root_clean] = reconciled.size();
        root_loaded[root_clean] = false;
        reconciled.push_back(record);
      } else {
        // A loaded or earlier phantom for the same root already represents it.
        changed = true;
      }
    }
  }

  if (changed) {
    session_config.notebooks = std::move(reconciled);
    config_manager_->SaveSessionConfig();
  }
}

VxCoreError NotebookManager::CreateNotebook(const std::string &root_folder, NotebookType type,
                                            const std::string &config_json,
                                            std::string &out_notebook_id) {
  VXCORE_LOG_INFO("Creating notebook: root_folder=%s, type=%d", root_folder.c_str(),
                  static_cast<int>(type));

  const auto root_folder_clean = CleanPath(root_folder);

  try {
    auto rootPath = PathFromUtf8(root_folder_clean);
    if (!std::filesystem::exists(rootPath)) {
      std::filesystem::create_directories(rootPath);
      VXCORE_LOG_DEBUG("Created root directory: %s", root_folder_clean.c_str());
    } else if (type == NotebookType::Bundled &&
               // Notebook::kConfigFileName is protected/unreachable here; use the
               // literal "config.json" (its value, defined in notebook.cpp).
               IsRegularFile(ConcatenatePaths(
                   ConcatenatePaths(root_folder_clean, BundledNotebook::kMetadataFolderName),
                   "config.json"))) {
      VXCORE_LOG_WARN(
          "CreateNotebook: '%s' already holds a bundled notebook; treating as re-add, "
          "preserving existing id",
          root_folder_clean.c_str());
      return OpenNotebook(root_folder, out_notebook_id);
    }

    const auto local_data_folder = config_manager_->GetLocalDataPath();

    // At least there should be name.
    assert(!config_json.empty());
    nlohmann::json json = nlohmann::json::parse(config_json);
    auto config = NotebookConfig::FromJson(json);
    config.id.clear();

    std::unique_ptr<Notebook> notebook;
    switch (type) {
      case NotebookType::Bundled: {
        auto err = BundledNotebook::Create(local_data_folder, root_folder_clean, &config, notebook);
        if (err != VXCORE_OK) {
          VXCORE_LOG_ERROR("Failed to create bundled notebook: root_folder=%s, error=%d",
                           root_folder_clean.c_str(), err);
          return err;
        }
        break;
      }
      case NotebookType::Raw: {
        auto err = RawNotebook::Create(local_data_folder, root_folder_clean, &config, notebook);
        if (err != VXCORE_OK) {
          VXCORE_LOG_ERROR("Failed to create raw notebook: root_folder=%s, error=%d",
                           root_folder_clean.c_str(), err);
          return err;
        }
        break;
      }
      default:
        VXCORE_LOG_ERROR("Invalid notebook type: %d", static_cast<int>(type));
        return VXCORE_ERR_INVALID_PARAM;
    }

    auto err = UpdateNotebookRecord(*notebook);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Failed to save notebook record after creation: id=%s, error=%d",
                       notebook->GetId().c_str(), err);
      return err;
    }

    out_notebook_id = notebook->GetId();
    if (event_manager_ && notebook->GetFolderManager()) {
      notebook->GetFolderManager()->SetEventManager(event_manager_);
      notebook->SetEventManager(event_manager_);
    }
    notebooks_[out_notebook_id] = std::move(notebook);

    VXCORE_LOG_INFO("Notebook created successfully: id=%s", out_notebook_id.c_str());
    if (event_manager_) {
      event_manager_->Emit(events::kNotebookOpened, {{kJsonKeyNotebookId, out_notebook_id}});
    }
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    VXCORE_LOG_ERROR("JSON parse error while creating notebook: %s", e.what());
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::filesystem::filesystem_error &e) {
    VXCORE_LOG_ERROR("Filesystem error while creating notebook: %s", e.what());
    return VXCORE_ERR_IO;
  } catch (...) {
    VXCORE_LOG_ERROR("Unknown error while creating notebook");
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::OpenNotebook(const std::string &root_folder,
                                          std::string &out_notebook_id) {
  VXCORE_LOG_INFO("Opening notebook: root_folder=%s", root_folder.c_str());
  const auto root_folder_clean = CleanPath(root_folder);
  if (auto *notebook = FindNotebookByRootFolder(root_folder_clean)) {
    out_notebook_id = notebook->GetId();
    VXCORE_LOG_DEBUG("Notebook already open: id=%s", out_notebook_id.c_str());
    return VXCORE_OK;
  }

  auto rootPath = PathFromUtf8(root_folder_clean);
  if (!std::filesystem::exists(rootPath)) {
    VXCORE_LOG_WARN("Notebook root folder not found: %s", root_folder_clean.c_str());
    return VXCORE_ERR_NOT_FOUND;
  }

  std::unique_ptr<Notebook> notebook;
  auto err =
      BundledNotebook::Open(config_manager_->GetLocalDataPath(), root_folder_clean, notebook);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to load bundled notebook: root_folder=%s, error=%d",
                     root_folder_clean.c_str(), err);
    return err;
  }

  err = UpdateNotebookRecord(*notebook);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to save notebook record after open: id=%s, error=%d",
                     notebook->GetId().c_str(), err);
    return err;
  }

  out_notebook_id = notebook->GetId();
  if (event_manager_ && notebook->GetFolderManager()) {
    notebook->GetFolderManager()->SetEventManager(event_manager_);
    notebook->SetEventManager(event_manager_);
  }
  notebooks_[out_notebook_id] = std::move(notebook);

  VXCORE_LOG_INFO("Notebook open successfully: id=%s", out_notebook_id.c_str());
  if (event_manager_) {
    event_manager_->Emit(events::kNotebookOpened, {{kJsonKeyNotebookId, out_notebook_id}});
  }
  return VXCORE_OK;
}

VxCoreError NotebookManager::CloseNotebook(const std::string &notebook_id) {
  VXCORE_LOG_INFO("Closing notebook: id=%s", notebook_id.c_str());

  auto it = notebooks_.find(notebook_id);
  if (it == notebooks_.end()) {
    VXCORE_LOG_WARN("Notebook not found for closing: id=%s", notebook_id.c_str());
    return VXCORE_ERR_NOT_FOUND;
  }

  // Capture the cleaned root before the runtime entry is erased so the session
  // record can be matched by root even when its persisted id is stale.
  const std::string root_clean = CleanPath(it->second->GetRootFolder());

  // Close notebook first to release DB file lock before deleting local data
  it->second->Close();

  DeleteNotebookLocalData(*it->second);

  notebooks_.erase(it);

  // Remove notebook record(s) from session config: match by id OR cleaned root,
  // so a record with a stale id (diverged from config.json) is still removed.
  auto &session_config = config_manager_->GetSessionConfig();
  session_config.notebooks.erase(
      std::remove_if(session_config.notebooks.begin(), session_config.notebooks.end(),
                     [&notebook_id, &root_clean](const NotebookRecord &r) {
                       return r.id == notebook_id || CleanPath(r.root_folder) == root_clean;
                     }),
      session_config.notebooks.end());

  config_manager_->SaveSessionConfig();

  VXCORE_LOG_INFO("Notebook closed successfully: id=%s", notebook_id.c_str());
  if (event_manager_) {
    event_manager_->Emit(events::kNotebookClosed, {{kJsonKeyNotebookId, notebook_id}});
  }
  return VXCORE_OK;
}

void NotebookManager::DeleteNotebookLocalData(const Notebook &notebook) {
  const auto local_data_folder = notebook.GetLocalDataFolder();

  if (std::filesystem::exists(PathFromUtf8(local_data_folder))) {
    VXCORE_LOG_INFO("Deleting notebook local data: id=%s, path=%s", notebook.GetId().c_str(),
                    local_data_folder.c_str());
    std::error_code ec;
    std::filesystem::remove_all(PathFromUtf8(local_data_folder), ec);
    if (ec) {
      VXCORE_LOG_ERROR("Failed to delete notebook local data: id=%s, path=%s, error=%s",
                       notebook.GetId().c_str(), local_data_folder.c_str(), ec.message().c_str());
    } else {
      VXCORE_LOG_DEBUG("Notebook local data deleted: id=%s, path=%s", notebook.GetId().c_str(),
                       local_data_folder.c_str());
    }
  }
}

VxCoreError NotebookManager::GetNotebookConfig(const std::string &notebook_id,
                                               std::string &out_config_json) {
  auto *notebook = GetNotebook(notebook_id);
  if (!notebook) {
    return VXCORE_ERR_NOT_FOUND;
  }

  try {
    nlohmann::json json = ToNotebookConfig(*notebook);
    out_config_json = json.dump();
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

nlohmann::json NotebookManager::ToNotebookConfig(const Notebook &notebook) const {
  nlohmann::json json = notebook.GetConfig().ToJson();
  json[kJsonKeyRootFolder] = notebook.GetRootFolder();
  json[kJsonKeyType] = notebook.GetTypeStr();
  return json;
}

VxCoreError NotebookManager::UpdateNotebookConfig(const std::string &notebook_id,
                                                  const std::string &config_json) {
  auto *notebook = GetNotebook(notebook_id);
  if (!notebook) {
    return VXCORE_ERR_NOT_FOUND;
  }

  try {
    nlohmann::json json = nlohmann::json::parse(config_json);
    NotebookConfig config = NotebookConfig::FromJson(json);
    config.id = notebook_id;

    VxCoreError err = notebook->UpdateConfig(config);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Failed to update notebook config: id=%s, error=%d", notebook_id.c_str(),
                       err);
      return err;
    }

    err = UpdateNotebookRecord(*notebook);
    if (err != VXCORE_OK) {
      VXCORE_LOG_ERROR("Failed to update notebook record after config update: id=%s, error=%d",
                       notebook_id.c_str(), err);
      return err;
    }

    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError NotebookManager::ListNotebooks(std::string &out_notebooks_json) {
  try {
    nlohmann::json jsonArray = nlohmann::json::array();

    for (const auto &pair : notebooks_) {
      nlohmann::json item = ToNotebookConfig(*pair.second);
      jsonArray.push_back(item);
    }

    out_notebooks_json = jsonArray.dump();
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

Notebook *NotebookManager::GetNotebook(const std::string &notebook_id) {
  auto it = notebooks_.find(notebook_id);
  if (it == notebooks_.end()) {
    return nullptr;
  }
  return it->second.get();
}

VxCoreError NotebookManager::UpdateNotebookRecord(const Notebook &notebook) {
  auto &session_config = config_manager_->GetSessionConfig();
  const std::string id = notebook.GetId();
  const std::string root_clean = CleanPath(notebook.GetRootFolder());

  // Remove every record that matches by id OR cleaned root, then upsert a single
  // canonical record. This guarantees exactly one record per (id, root) and heals
  // a pre-existing duplicate or id/root mismatch on the next write.
  session_config.notebooks.erase(
      std::remove_if(session_config.notebooks.begin(), session_config.notebooks.end(),
                     [&id, &root_clean](const NotebookRecord &r) {
                       return r.id == id || CleanPath(r.root_folder) == root_clean;
                     }),
      session_config.notebooks.end());

  session_config.notebooks.emplace_back();
  NotebookRecord *record = &session_config.notebooks.back();
  record->id = notebook.GetId();
  record->root_folder = notebook.GetRootFolder();
  record->type = notebook.GetType();
  // T14: persist the per-device read-only flag alongside the rest of the
  // record so subsequent session-restore (T15's LoadOpenNotebooks change)
  // can re-apply it via Notebook::SetReadOnly. Bundled and Raw notebooks
  // both participate; the runtime flag lives on the base class.
  record->read_only = notebook.IsReadOnly();

  config_manager_->SaveSessionConfig();
  return VXCORE_OK;
}

void NotebookManager::RecordNotebookReadOnly(const std::string &notebook_id, bool read_only) {
  // T14: best-effort persistence of the per-device RO flag. The runtime
  // flag has already been mutated on the Notebook by the caller via
  // SetReadOnly; this method just mirrors it into the persisted
  // NotebookRecord so the next session restore picks it up.
  auto *notebook = GetNotebook(notebook_id);
  if (!notebook) {
    VXCORE_LOG_WARN("RecordNotebookReadOnly: notebook not found id=%s", notebook_id.c_str());
    return;
  }
  // Defensive: callers should have already called SetReadOnly, but to make
  // this method usable from any path (T14 calls it after SetReadOnly; future
  // callers might pass through) we honor the explicit flag rather than
  // re-reading notebook->IsReadOnly().
  notebook->SetReadOnly(read_only);
  // UpdateNotebookRecord reads notebook->IsReadOnly() into record->read_only
  // and rewrites session.json. Failure (e.g. disk full) logs through the
  // SaveSessionConfig path -- we don't propagate because the runtime flag
  // is already correct and the caller (vxcore_notebook_open_ex) has already
  // succeeded; failing here would orphan the registered notebook.
  VxCoreError err = UpdateNotebookRecord(*notebook);
  if (err != VXCORE_OK) {
    VXCORE_LOG_WARN("RecordNotebookReadOnly: UpdateNotebookRecord failed for id=%s, err=%d",
                    notebook_id.c_str(), err);
  }
}

NotebookRecord *NotebookManager::FindNotebookRecord(const std::string &id) {
  auto &session_config = config_manager_->GetSessionConfig();
  auto it = std::find_if(session_config.notebooks.begin(), session_config.notebooks.end(),
                         [&id](const NotebookRecord &r) { return r.id == id; });

  if (it != session_config.notebooks.end()) {
    return &(*it);
  }

  return nullptr;
}

Notebook *NotebookManager::FindNotebookByRootFolder(const std::string &root_folder) {
  for (const auto &pair : notebooks_) {
    if (pair.second->GetRootFolder() == root_folder) {
      return pair.second.get();
    }
  }
  return nullptr;
}

VxCoreError NotebookManager::ResolvePathToNotebook(const std::string &absolute_path,
                                                   std::string &out_notebook_id,
                                                   std::string &out_relative_path) {
  const std::string clean_path = CleanPath(absolute_path);
  auto abs_path = PathFromUtf8(clean_path);

  // Iterate through all open notebooks and find which one contains this path.
  for (const auto &pair : notebooks_) {
    const std::string &root_folder = pair.second->GetRootFolder();
    auto root_path = PathFromUtf8(root_folder);

    // Check if absolute_path starts with root_folder.
    auto [root_end, path_pos] = std::mismatch(root_path.begin(), root_path.end(), abs_path.begin());

    if (root_end == root_path.end()) {
      // Path is within this notebook.
      out_notebook_id = pair.first;

      // Compute relative path.
      std::filesystem::path relative;
      for (; path_pos != abs_path.end(); ++path_pos) {
        relative /= *path_pos;
      }
      out_relative_path = CleanFsPath(relative);

      VXCORE_LOG_DEBUG("Resolved path %s to notebook %s, relative path %s", clean_path.c_str(),
                       out_notebook_id.c_str(), out_relative_path.c_str());
      return VXCORE_OK;
    }
  }

  VXCORE_LOG_DEBUG("Path %s not found in any open notebook", clean_path.c_str());
  return VXCORE_ERR_NOT_FOUND;
}

VxCoreError NotebookManager::ResolveNodeById(const std::string &node_id,
                                             std::string &out_notebook_id,
                                             std::string &out_relative_path) {
  for (const auto &pair : notebooks_) {
    auto *store = pair.second->GetMetadataStore();
    if (!store) {
      continue;
    }

    std::string path = store->GetNodePathById(node_id);
    if (!path.empty()) {
      out_notebook_id = pair.first;
      out_relative_path = path;
      VXCORE_LOG_DEBUG("Resolved node %s to notebook %s, relative path %s", node_id.c_str(),
                       out_notebook_id.c_str(), out_relative_path.c_str());
      return VXCORE_OK;
    }
  }

  VXCORE_LOG_DEBUG("Node %s not found in any open notebook", node_id.c_str());
  return VXCORE_ERR_NOT_FOUND;
}

void NotebookManager::SetEventManager(EventManager *event_manager) {
  event_manager_ = event_manager;
  // Propagate to existing notebooks and their folder managers
  for (auto &pair : notebooks_) {
    if (!pair.second) {
      continue;
    }
    pair.second->SetEventManager(event_manager_);
    if (auto *fm = pair.second->GetFolderManager()) {
      fm->SetEventManager(event_manager_);
    }
  }
}

}  // namespace vxcore
