#include <stdlib.h>
#include <string.h>
#include <vxcore/notebook_json_keys.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <string>
#include <stdexcept>

#include "api/api_utils.h"
#include "core/buffer_manager.h"
#include "core/bundled_folder_manager.h"
#include "core/bundled_notebook.h"
#include "core/config_manager.h"
#include "core/context.h"
#include "core/event_manager.h"
#include "core/event_names.h"
#include "core/history_manager.h"
#include "core/metadata_store.h"
#include "core/notebook.h"
#include "core/notebook_manager.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "vxcore/vxcore.h"

namespace {

template <typename Function>
VxCoreError EncryptionApiResult(Function &&function) {
  // Do not write context.last_error here: independent worker KDFs may execute
  // concurrently. Consumers obtain stable public text from vxcore_error_message().
  try {
    return function();
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::length_error &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_ENCRYPTION_FORMAT;
  } catch (const std::filesystem::filesystem_error &) {
    return VXCORE_ERR_IO;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

}  // namespace

VXCORE_API VxCoreError vxcore_encryption_prepare_notebook(
    VxCoreContextHandle context, const char *notebook_id, const char *source_notebook_id,
    const void *password, size_t password_size, VxCoreEncryptionSetupHandle *out_setup) {
  if (out_setup) {
    *out_setup = nullptr;
  }
  if (!context || !notebook_id || !out_setup || (!password && password_size)) {
    return VXCORE_ERR_NULL_POINTER;
  }
  if (!*notebook_id || (source_notebook_id && !*source_notebook_id)) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  return EncryptionApiResult([&]() {
    vxcore::NotebookManager::EncryptionSetup *setup = nullptr;
    auto error = ctx->notebook_manager->PrepareNotebookEncryption(
        notebook_id, source_notebook_id ? source_notebook_id : "", password, password_size, setup);
    if (error == VXCORE_OK) {
      *out_setup = reinterpret_cast<VxCoreEncryptionSetupHandle>(setup);
    }
    return error;
  });
}

VXCORE_API VxCoreError vxcore_encryption_commit_notebook(
    VxCoreContextHandle context, VxCoreEncryptionSetupHandle setup) {
  if (!context) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  return EncryptionApiResult([&]() {
    return ctx->notebook_manager->CommitNotebookEncryption(
        reinterpret_cast<vxcore::NotebookManager::EncryptionSetup *>(setup));
  });
}

VXCORE_API void vxcore_encryption_free_setup(
    VxCoreContextHandle context, VxCoreEncryptionSetupHandle setup) {
  if (!context || !setup) {
    return;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  (void)EncryptionApiResult([&]() {
    ctx->notebook_manager->FreeEncryptionSetup(
        reinterpret_cast<vxcore::NotebookManager::EncryptionSetup *>(setup));
    return VXCORE_OK;
  });
}

VXCORE_API VxCoreError vxcore_encryption_unlock_notebook(
    VxCoreContextHandle context, const char *notebook_id,
    const void *password, size_t password_size) {
  if (!context || !notebook_id || (!password && password_size)) {
    return VXCORE_ERR_NULL_POINTER;
  }
  if (!*notebook_id) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  return EncryptionApiResult([&]() {
    return ctx->notebook_manager->UnlockNotebookEncryption(notebook_id, password, password_size);
  });
}

VXCORE_API VxCoreError vxcore_encryption_lock_all(VxCoreContextHandle context) {
  if (!context) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (ctx->buffer_manager && ctx->buffer_manager->HasProtectedBuffers()) {
    return VXCORE_ERR_INVALID_STATE;
  }
  return EncryptionApiResult([&]() { return ctx->notebook_manager->LockAllEncryption(); });
}

VXCORE_API VxCoreError vxcore_encryption_get_status(
    VxCoreContextHandle context, const char *notebook_id,
    const char *file_path, char **out_status_json) {
  if (out_status_json) {
    *out_status_json = nullptr;
  }
  if (!context || !notebook_id || !out_status_json) {
    return VXCORE_ERR_NULL_POINTER;
  }
  if (!*notebook_id) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  return EncryptionApiResult([&]() {
    std::string json;
    auto error = ctx->notebook_manager->GetEncryptionStatus(notebook_id, file_path, json);
    if (error == VXCORE_OK) {
      *out_status_json = vxcore_strdup(json.c_str());
      if (!*out_status_json) {
        return VXCORE_ERR_OUT_OF_MEMORY;
      }
    }
    return error;
  });
}

VXCORE_API VxCoreError vxcore_encryption_protect_note(
    VxCoreContextHandle context, const char *notebook_id, const char *file_path,
    const void *body, size_t body_size, const char *source_sha256,
    char **out_encrypted_path) {
  if (out_encrypted_path) {
    *out_encrypted_path = nullptr;
  }
  if (!context || !notebook_id || !file_path || !source_sha256 ||
      !out_encrypted_path || (!body && body_size)) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  return EncryptionApiResult([&]() -> VxCoreError {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      return VXCORE_ERR_NOT_FOUND;
    }
    auto *manager = dynamic_cast<vxcore::BundledFolderManager *>(notebook->GetFolderManager());
    if (!manager) {
      return VXCORE_ERR_UNSUPPORTED;
    }
    // Reserve ABI output before any durable publication.
    const auto target = notebook->GetCleanRelativePath(file_path) + ".vne";
    std::unique_ptr<char, decltype(&std::free)> result(vxcore_strdup(target.c_str()), &std::free);
    if (!result) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }
    std::string path;
    const auto error = manager->ProtectNote(file_path, body, body_size, source_sha256,
                                             ctx->config_manager->GetConfig().file_types, path);
    if (error == VXCORE_OK) {
      *out_encrypted_path = result.release();
    }
    return error;
  });
}

VXCORE_API VxCoreError vxcore_encryption_create_note(
    VxCoreContextHandle context, const char *notebook_id, const char *parent_path,
    const char *name, const char *editor_type, const void *body, size_t body_size,
    char **out_file_id) {
  if (out_file_id) {
    *out_file_id = nullptr;
  }
  if (!context || !notebook_id || !parent_path || !name || !editor_type ||
      !out_file_id || (!body && body_size)) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  return EncryptionApiResult([&]() -> VxCoreError {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      return VXCORE_ERR_NOT_FOUND;
    }
    auto *manager = dynamic_cast<vxcore::BundledFolderManager *>(notebook->GetFolderManager());
    if (!manager) {
      return VXCORE_ERR_UNSUPPORTED;
    }
    std::unique_ptr<char, decltype(&std::free)> result(
        static_cast<char *>(std::malloc(37)), &std::free);
    if (!result) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }
    std::string id;
    const auto error = manager->CreateEncryptedNote(parent_path, name, editor_type,
                                                      body, body_size,
                                                      ctx->config_manager->GetConfig().file_types, id);
    if (error == VXCORE_OK) {
      std::memcpy(result.get(), id.c_str(), 37);
      *out_file_id = result.release();
    }
    return error;
  });
}

struct VxCoreRecycleBinCleanup {
  std::string notebook_id;
  std::string recycle_bin_path;
  int64_t cutoff_utc_ms = 0;
  bool path_inside_notebook_root = false;
  vxcore::EventManager *event_manager = nullptr;
  std::atomic_bool cancelled{false};
  std::atomic_bool executed{false};
};
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
                                               const char *options_json, char **out_notebook_id) {
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

VXCORE_API VxCoreError vxcore_notebook_prepare_recycle_bin_cleanup(
    VxCoreContextHandle context, const char *notebook_id, int64_t cutoff_utc_ms,
    VxCoreRecycleBinCleanup **out_cleanup) {
  if (!context || !notebook_id || !out_cleanup) {
    return VXCORE_ERR_NULL_POINTER;
  }
  *out_cleanup = nullptr;
  if (cutoff_utc_ms < 0) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }
    if (notebook->IsReadOnly()) {
      ctx->last_error = "Notebook is read-only";
      return VXCORE_ERR_READ_ONLY;
    }
    if (notebook->GetType() != vxcore::NotebookType::Bundled) {
      ctx->last_error = "Recycle bin cleanup requires a bundled notebook";
      return VXCORE_ERR_UNSUPPORTED;
    }

    const std::string recycle_bin_path = notebook->GetRecycleBinPath();
    const std::string root_path = notebook->GetRootFolder();
    std::error_code root_ec;
    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(vxcore::PathFromUtf8(root_path), root_ec);
    std::error_code recycle_ec;
    const std::filesystem::path canonical_recycle =
        std::filesystem::weakly_canonical(vxcore::PathFromUtf8(recycle_bin_path), recycle_ec);
    if (root_ec || recycle_ec || canonical_root.empty() || canonical_recycle.empty()) {
      ctx->last_error = "Failed to resolve recycle bin path";
      return VXCORE_ERR_IO;
    }
    if (canonical_recycle == canonical_root) {
      ctx->last_error = "Recycle bin path cannot equal the notebook root";
      return VXCORE_ERR_INVALID_PARAM;
    }

    std::unique_ptr<VxCoreRecycleBinCleanup> cleanup(new (std::nothrow) VxCoreRecycleBinCleanup());
    if (!cleanup) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }
    cleanup->notebook_id = notebook_id;
    cleanup->recycle_bin_path = vxcore::PathToUtf8(canonical_recycle);
    cleanup->cutoff_utc_ms = cutoff_utc_ms;
    cleanup->path_inside_notebook_root =
        vxcore::IsPathWithinCanonical(canonical_root, cleanup->recycle_bin_path, false);
    cleanup->event_manager = ctx->event_manager.get();
    *out_cleanup = cleanup.release();
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error preparing recycle bin cleanup";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_recycle_bin_cleanup_execute(VxCoreRecycleBinCleanup *cleanup,
                                                          int *out_removed_count) {
  if (out_removed_count) {
    *out_removed_count = 0;
  }
  if (!cleanup) {
    return VXCORE_ERR_NULL_POINTER;
  }
  if (cleanup->executed.exchange(true, std::memory_order_acq_rel)) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  try {
    int removed_count = 0;
    bool changed = false;
    const VxCoreError error = vxcore::BundledNotebook::CleanupRecycleBinPath(
        cleanup->recycle_bin_path, cleanup->cutoff_utc_ms, cleanup->cancelled, &removed_count,
        &changed);
    if (out_removed_count) {
      *out_removed_count = removed_count;
    }
    if (changed && cleanup->path_inside_notebook_root && cleanup->event_manager) {
      cleanup->event_manager->Emit(
          vxcore::events::kRecycleBinCleaned,
          {{vxcore::kJsonKeyNotebookId, cleanup->notebook_id}, {"removedCount", removed_count}});
    }
    return error;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API void vxcore_recycle_bin_cleanup_cancel(VxCoreRecycleBinCleanup *cleanup) {
  if (cleanup) {
    cleanup->cancelled.store(true, std::memory_order_relaxed);
  }
}

VXCORE_API void vxcore_recycle_bin_cleanup_free(VxCoreRecycleBinCleanup *cleanup) {
  delete cleanup;
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
    VXCORE_LOG_DEBUG("history_get_resolved: %zu entries from store for notebook %s", history.size(),
                     notebook_id);
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
                                                     const char *notebook_id, bool read_only) {
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
                                                    const char *notebook_id, bool *out_read_only) {
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
