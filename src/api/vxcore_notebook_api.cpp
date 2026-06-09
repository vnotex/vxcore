#include <stdlib.h>
#include <string.h>

#include <nlohmann/json.hpp>
#include <vxcore/notebook_json_keys.h>

#include "api/api_utils.h"
#include "core/buffer_manager.h"
#include "core/context.h"
#include "core/history_manager.h"
#include "core/metadata_store.h"
#include "core/notebook.h"
#include "core/notebook_manager.h"
#include "utils/logger.h"
#include "vxcore/vxcore.h"

VXCORE_API VxCoreError vxcore_notebook_create(VxCoreContextHandle context, const char *path,
                                              const char *config_json, VxCoreNotebookType type,
                                              char **out_notebook_id) {
  if (!context || !path || !out_notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::NotebookType notebook_type =
        (type == VXCORE_NOTEBOOK_RAW) ? vxcore::NotebookType::Raw : vxcore::NotebookType::Bundled;

    std::string config_str = config_json ? config_json : "";
    std::string notebook_id;

    VxCoreError err =
        ctx->notebook_manager->CreateNotebook(path, notebook_type, config_str, notebook_id);

    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to create notebook";
      return err;
    }

    char *id_copy = vxcore_strdup(notebook_id.c_str());
    if (!id_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_notebook_id = id_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error creating notebook";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_open(VxCoreContextHandle context, const char *path,
                                            char **out_notebook_id) {
  // T14 of open-notebook-remote-readonly: legacy entry point is now a
  // 1-line shim around the extended variant. Back-compat is guaranteed by
  // construction -- the only behaviour difference vs the pre-T14 body is
  // that NotebookRecord.read_only is now explicitly persisted as false
  // (which was its default before T14 anyway), so existing callers see no
  // observable change.
  return vxcore_notebook_open_ex(context, path, "{}", out_notebook_id);
}

VXCORE_API VxCoreError vxcore_notebook_open_ex(VxCoreContextHandle context, const char *path,
                                                const char *options_json,
                                                char **out_notebook_id) {
  if (!context || !path || !out_notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  // Defensive: documented "no notebook registered on parse failure" contract
  // requires the out-pointer to be cleared up front so every early-return
  // path is honest without extra bookkeeping. The success path overwrites
  // this at the end.
  *out_notebook_id = nullptr;

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  // Step 1: parse + validate options BEFORE touching NotebookManager. The
  // null / empty string case is treated as "{}", which is the documented
  // pre-T14 default and lets the shim above stay a 1-liner. Anything that
  // either fails to parse OR is not a JSON object (e.g. a top-level array
  // "[]") is rejected with VXCORE_ERR_JSON_PARSE so callers can't sneak
  // unintended types through.
  //
  // CRITICAL (MSVC + nlohmann unwind quirk): use the no-throw parse
  // overload (allow_exceptions=false + is_discarded() check) instead of
  // letting json::exception propagate out of vxcore.dll. The throwing
  // path wedges the test process on Windows when /EHsc isn't on every
  // TU; the no-throw overload sidesteps the issue entirely. See
  // tests/test_notebook_open_ex.cpp's FindRecordInSession helper for
  // the same workaround.
  bool read_only = false;
  const char *opts_to_parse = options_json;
  if (opts_to_parse == nullptr || opts_to_parse[0] == '\0') {
    opts_to_parse = "{}";
  }
  auto opts_json = nlohmann::json::parse(opts_to_parse, /*cb=*/nullptr,
                                          /*allow_exceptions=*/false);
  if (opts_json.is_discarded()) {
    ctx->last_error = "vxcore_notebook_open_ex: options_json malformed";
    return VXCORE_ERR_JSON_PARSE;
  }
  if (!opts_json.is_object()) {
    ctx->last_error = "vxcore_notebook_open_ex: options_json must be a JSON object";
    return VXCORE_ERR_JSON_PARSE;
  }
  // .value() returns the default if the key is absent or not the right
  // type; for our forward-compat "unknown keys ignored" semantics that's
  // exactly what we want.
  read_only = opts_json.value(vxcore::kJsonKeyReadOnly, false);

  // Step 2: open the notebook through the existing NotebookManager pipeline.
  // OpenNotebook handles dedup-by-root-folder, BundledNotebook::Open, and
  // the initial UpdateNotebookRecord call. The recorded NotebookRecord at
  // this point has read_only=false because we haven't flipped the runtime
  // flag yet.
  std::string notebook_id;
  try {
    VxCoreError err = ctx->notebook_manager->OpenNotebook(path, notebook_id);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to open notebook";
      return err;
    }
  } catch (...) {
    ctx->last_error = "Unknown error opening notebook";
    return VXCORE_ERR_UNKNOWN;
  }

  // Step 3: apply the requested RO state and re-persist the NotebookRecord
  // so a downstream restart restores the same flag (T15 closes that loop).
  // We only re-record when the user actually asked for read-only -- the
  // false case is already persisted by the OpenNotebook call above (the
  // record was just created with the default).
  if (read_only) {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (notebook != nullptr) {
      notebook->SetReadOnly(true);
      // RecordNotebookReadOnly persists the new flag value to session.json
      // by re-running the NotebookRecord update path. The method swallows
      // I/O failures (consistent with SetLastSyncUtc semantics) -- not
      // having the flag persisted is recoverable on next open_ex; failing
      // the API call here would orphan a successfully-opened notebook.
      ctx->notebook_manager->RecordNotebookReadOnly(notebook_id, true);
    }
  }

  char *id_copy = vxcore_strdup(notebook_id.c_str());
  if (!id_copy) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  }

  *out_notebook_id = id_copy;
  return VXCORE_OK;
}

VXCORE_API VxCoreError vxcore_notebook_close(VxCoreContextHandle context, const char *notebook_id) {
  if (!context || !notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    // Close all buffers associated with this notebook first
    if (ctx->buffer_manager) {
      ctx->buffer_manager->CloseBuffersForNotebook(notebook_id);
    }

    VxCoreError err = ctx->notebook_manager->CloseNotebook(notebook_id);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to close notebook";
    }
    return err;
  } catch (...) {
    ctx->last_error = "Unknown error closing notebook";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_list(VxCoreContextHandle context,
                                            char **out_notebooks_json) {
  if (!context || !out_notebooks_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    std::string notebooks_json;
    VxCoreError err = ctx->notebook_manager->ListNotebooks(notebooks_json);

    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to list notebooks";
      return err;
    }

    char *json_copy = vxcore_strdup(notebooks_json.c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_notebooks_json = json_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error listing notebooks";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_get_config(VxCoreContextHandle context,
                                                  const char *notebook_id, char **out_config_json) {
  if (!context || !notebook_id || !out_config_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    std::string config_json;
    VxCoreError err = ctx->notebook_manager->GetNotebookConfig(notebook_id, config_json);

    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to get notebook config";
      return err;
    }

    char *json_copy = vxcore_strdup(config_json.c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_config_json = json_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error getting notebook config";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_update_config(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     const char *config_json) {
  if (!context || !notebook_id || !config_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    VxCoreError err = ctx->notebook_manager->UpdateNotebookConfig(notebook_id, config_json);

    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to set notebook config";
    }
    return err;
  } catch (...) {
    ctx->last_error = "Unknown error setting notebook config";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_rebuild_cache(VxCoreContextHandle context,
                                                     const char *notebook_id) {
  if (!context || !notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    VxCoreError err = notebook->RebuildCache();
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to rebuild notebook cache";
    }
    return err;
  } catch (...) {
    ctx->last_error = "Unknown error rebuilding notebook cache";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_get_recycle_bin_path(VxCoreContextHandle context,
                                                            const char *notebook_id,
                                                            char **out_path) {
  if (!context || !notebook_id || !out_path) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    std::string path = notebook->GetRecycleBinPath();
    char *path_copy = vxcore_strdup(path.c_str());
    if (!path_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_path = path_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error getting recycle bin path";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_empty_recycle_bin(VxCoreContextHandle context,
                                                         const char *notebook_id) {
  if (!context || !notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    VxCoreError err = notebook->EmptyRecycleBin();
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to empty recycle bin";
    }
    return err;
  } catch (...) {
    ctx->last_error = "Unknown error emptying recycle bin";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_path_resolve(VxCoreContextHandle context, const char *absolute_path,
                                           char **out_notebook_id, char **out_relative_path) {
  if (!context || !absolute_path || !out_notebook_id || !out_relative_path) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    std::string notebook_id;
    std::string relative_path;
    VxCoreError err =
        ctx->notebook_manager->ResolvePathToNotebook(absolute_path, notebook_id, relative_path);

    if (err != VXCORE_OK) {
      ctx->last_error = "Path not found in any open notebook";
      return err;
    }

    char *id_copy = vxcore_strdup(notebook_id.c_str());
    if (!id_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    char *path_copy = vxcore_strdup(relative_path.c_str());
    if (!path_copy) {
      free(id_copy);
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_notebook_id = id_copy;
    *out_relative_path = path_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error resolving path";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_path_build_absolute(VxCoreContextHandle context,
                                                  const char *notebook_id,
                                                  const char *relative_path,
                                                  char **out_absolute_path) {
  if (!context || !notebook_id || !relative_path || !out_absolute_path) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found: " + std::string(notebook_id);
      return VXCORE_ERR_NOT_FOUND;
    }

    std::string abs_path = notebook->GetAbsolutePath(relative_path);
    char *path_copy = vxcore_strdup(abs_path.c_str());
    if (!path_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_absolute_path = path_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error building absolute path";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_history_get(VxCoreContextHandle context,
                                                   const char *notebook_id,
                                                   char **out_history_json) {
  if (!context || !notebook_id || !out_history_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    auto *store = notebook->GetMetadataStore();
    if (!store) {
      ctx->last_error = "Metadata store not available";
      return VXCORE_ERR_INVALID_STATE;
    }

    auto history = vxcore::GetHistory(store);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &entry : history) {
      arr.push_back(entry.ToJson());
    }

    char *json_copy = vxcore_strdup(arr.dump().c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_history_json = json_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error getting notebook history";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_history_clear(VxCoreContextHandle context,
                                                     const char *notebook_id) {
  if (!context || !notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    auto *store = notebook->GetMetadataStore();
    if (!store) {
      ctx->last_error = "Metadata store not available";
      return VXCORE_ERR_INVALID_STATE;
    }

    if (!vxcore::ClearHistory(store)) {
      ctx->last_error = "Failed to clear notebook history";
      return VXCORE_ERR_DATABASE;
    }

    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error clearing notebook history";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_history_get_resolved(VxCoreContextHandle context,
                                                             const char *notebook_id,
                                                             char **out_history_json) {
  if (!context || !notebook_id || !out_history_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    auto *store = notebook->GetMetadataStore();
    if (!store) {
      ctx->last_error = "Metadata store not available";
      return VXCORE_ERR_INVALID_STATE;
    }

    auto history = vxcore::GetHistory(store);
    VXCORE_LOG_DEBUG("history_get_resolved: %zu entries from store for notebook %s",
                    history.size(), notebook_id);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &entry : history) {
      auto path = store->GetNodePathById(entry.file_id);
      VXCORE_LOG_DEBUG("history_get_resolved: file_id=%s -> path=%s", entry.file_id.c_str(),
                      path.empty() ? "DROPPED" : path.c_str());
      if (path.empty()) {
        continue;
      }
      auto pos = path.rfind('/');
      auto name = (pos != std::string::npos) ? path.substr(pos + 1) : path;
      nlohmann::json obj;
      obj["fileId"] = entry.file_id;
      obj["openedUtc"] = entry.opened_utc;
      obj["relativePath"] = path;
      obj["name"] = name;
      arr.push_back(obj);
    }

    char *json_copy = vxcore_strdup(arr.dump().c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

     *out_history_json = json_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error getting resolved notebook history";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_set_read_only(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     bool read_only) {
  if (!context || !notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    notebook->SetReadOnly(read_only);
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error setting notebook read-only flag";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_is_read_only(VxCoreContextHandle context,
                                                    const char *notebook_id,
                                                    bool *out_read_only) {
  if (!context || !notebook_id || !out_read_only) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    *out_read_only = notebook->IsReadOnly();
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error getting notebook read-only flag";
    return VXCORE_ERR_UNKNOWN;
  }
}

