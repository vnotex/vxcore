#include <nlohmann/json.hpp>

#include "api/api_utils.h"
#include "core/context.h"
#include "core/notebook.h"
#include "core/notebook_manager.h"
#include "sync/credential_provider.h"
#include "sync/sync_cancellation.h"
#include "sync/sync_json_keys.h"
#include "sync/sync_manager.h"
#include "sync/sync_types.h"
#include "utils/logger.h"
#include "vxcore/vxcore.h"

// Wave 12.2 / F5.9: opaque C handle wrapping a shared_ptr<SyncCancellation>.
// Defined here (not in a public header) so the layout stays an implementation
// detail. The C API exposes only a forward-declared typedef.
struct VxCoreSyncCancellation_ {
  vxcore::SyncCancellationPtr ptr;
};

static const char *SyncStateToString(vxcore::SyncState state) {
  switch (state) {
    case vxcore::SyncState::kIdle: return "idle";
    case vxcore::SyncState::kStaging: return "staging";
    case vxcore::SyncState::kFetching: return "fetching";
    case vxcore::SyncState::kAnalyzing: return "analyzing";
    case vxcore::SyncState::kMerging: return "merging";
    case vxcore::SyncState::kPushing: return "pushing";
    case vxcore::SyncState::kError: return "error";
    case vxcore::SyncState::kConflicted: return "conflicted";
  }
  return "unknown";
}

static const char *SyncFileStatusToString(vxcore::SyncFileStatus status) {
  switch (status) {
    case vxcore::SyncFileStatus::kUnchanged: return "unchanged";
    case vxcore::SyncFileStatus::kModifiedLocal: return "modified_local";
    case vxcore::SyncFileStatus::kModifiedRemote: return "modified_remote";
    case vxcore::SyncFileStatus::kConflicted: return "conflicted";
    case vxcore::SyncFileStatus::kAddedLocal: return "added_local";
    case vxcore::SyncFileStatus::kAddedRemote: return "added_remote";
    case vxcore::SyncFileStatus::kDeletedLocal: return "deleted_local";
    case vxcore::SyncFileStatus::kDeletedRemote: return "deleted_remote";
  }
  return "unknown";
}

// Parse credentials_json into a vxcore::SyncCredentials. All fields optional;
// missing or non-string fields become empty strings. Throws nlohmann::json::exception
// on malformed JSON (caught by callers).
static vxcore::SyncCredentials ParseCredentials(const char *credentials_json) {
  vxcore::SyncCredentials creds;
  auto j = nlohmann::json::parse(credentials_json);
  if (j.contains(vxcore::kJsonKeyPat) && j[vxcore::kJsonKeyPat].is_string()) {
    creds.personal_access_token = j[vxcore::kJsonKeyPat].get<std::string>();
  }
  if (j.contains(vxcore::kJsonKeyAuthorName) && j[vxcore::kJsonKeyAuthorName].is_string()) {
    creds.author_name = j[vxcore::kJsonKeyAuthorName].get<std::string>();
  }
  if (j.contains(vxcore::kJsonKeyAuthorEmail) && j[vxcore::kJsonKeyAuthorEmail].is_string()) {
    creds.author_email = j[vxcore::kJsonKeyAuthorEmail].get<std::string>();
  }
  if (j.contains(vxcore::kJsonKeyExtra) && j[vxcore::kJsonKeyExtra].is_object()) {
    creds.extra = j[vxcore::kJsonKeyExtra];
  }
  return creds;
}

VXCORE_API VxCoreError vxcore_sync_enable(VxCoreContextHandle context, const char *notebook_id,
                                          const char *config_json,
                                          const char *credentials_json) {
  // credentials_json is intentionally nullable; only context/notebook_id/config_json are required.
  if (!context || !notebook_id || !config_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    if (!ctx->sync_manager) {
      ctx->last_error = "Sync manager not initialized";
      return VXCORE_ERR_UNKNOWN;
    }

    auto j = nlohmann::json::parse(config_json);

    vxcore::SyncConfig config = vxcore::SyncConfig::FromJson(j);

    // Build the credential provider. NEVER log the PAT value itself.
    // Wave 7.1 (sync-backend-phase4): credentials_json == nullptr installs a
    // NoOpCredentialProvider so the AuthRequired guard in SyncManager sees a
    // non-null provider while still declining at the libgit2 callback for
    // backends that genuinely need creds — preserving the legacy creds-less
    // behaviour.
    std::shared_ptr<vxcore::ICredentialProvider> provider;
    if (credentials_json != nullptr) {
      vxcore::SyncCredentials creds = ParseCredentials(credentials_json);
      provider =
          std::make_shared<vxcore::InMemoryCredentialProvider>(std::move(creds));
    } else {
      provider = std::make_shared<vxcore::NoOpCredentialProvider>();
    }

    return ctx->sync_manager->EnableSync(notebook_id, config, std::move(provider));
  } catch (const nlohmann::json::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error enabling sync";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_sync_disable(VxCoreContextHandle context, const char *notebook_id) {
  if (!context || !notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    if (!ctx->sync_manager) {
      ctx->last_error = "Sync manager not initialized";
      return VXCORE_ERR_UNKNOWN;
    }

    return ctx->sync_manager->DisableSync(notebook_id);
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error disabling sync";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_sync_trigger(VxCoreContextHandle context, const char *notebook_id) {
  if (!context || !notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    if (!ctx->sync_manager) {
      ctx->last_error = "Sync manager not initialized";
      return VXCORE_ERR_UNKNOWN;
    }

    return ctx->sync_manager->TriggerSync(notebook_id);
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error triggering sync";
    return VXCORE_ERR_UNKNOWN;
  }
}

// Wave 12.2 / F5.9: cancellation token lifecycle. All three handle
// management fns are null-safe.
VXCORE_API VxCoreSyncCancellation *vxcore_sync_create_cancellation(void) {
  try {
    auto *handle = new VxCoreSyncCancellation_;
    handle->ptr = std::make_shared<vxcore::SyncCancellation>();
    return handle;
  } catch (...) {
    return nullptr;
  }
}

VXCORE_API void vxcore_sync_cancel(VxCoreSyncCancellation *token) {
  if (!token || !token->ptr) {
    return;
  }
  token->ptr->Cancel();
}

VXCORE_API void vxcore_sync_free_cancellation(VxCoreSyncCancellation *token) {
  // Releasing the handle drops the outer shared_ptr ref; the underlying
  // SyncCancellation object stays alive as long as any in-flight pipeline
  // still holds a snapshot of it. No leak — pipelines drop their snapshot
  // when Sync() returns and SyncManager calls backend->SetCancellation(nullptr).
  delete token;
}

VXCORE_API VxCoreError vxcore_sync_trigger_cancellable(VxCoreContextHandle context,
                                                       const char *notebook_id,
                                                       VxCoreSyncCancellation *token) {
  if (!context || !notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    if (!ctx->sync_manager) {
      ctx->last_error = "Sync manager not initialized";
      return VXCORE_ERR_UNKNOWN;
    }

    // Null token degrades to the legacy non-cancellable path. Non-null:
    // forward the shared_ptr (copy, so the C handle stays free to outlive
    // this call — pipelines snapshot internally).
    vxcore::SyncCancellationPtr ptr = (token != nullptr) ? token->ptr : nullptr;
    return ctx->sync_manager->TriggerSync(notebook_id, std::move(ptr));
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error triggering cancellable sync";
    return VXCORE_ERR_UNKNOWN;
  }
}

// vxcore-sync-stage-only V1: stage-only and network-phase C entries.
// Mirror the exception-handling shape of vxcore_sync_trigger_cancellable;
// out_did_commit is optional (may be NULL).
VXCORE_API VxCoreError vxcore_sync_stage_only(VxCoreContextHandle context,
                                              const char *notebook_id,
                                              VxCoreSyncCancellation *token,
                                              int *out_did_commit) {
  if (!context || !notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    if (!ctx->sync_manager) {
      ctx->last_error = "Sync manager not initialized";
      return VXCORE_ERR_UNKNOWN;
    }

    vxcore::SyncCancellationPtr ptr = (token != nullptr) ? token->ptr : nullptr;
    bool did_commit = false;
    VxCoreError err =
        ctx->sync_manager->StageOnly(notebook_id, std::move(ptr), &did_commit);
    if (out_did_commit != nullptr) {
      *out_did_commit = did_commit ? 1 : 0;
    }
    return err;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error running stage-only sync";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_sync_network_phase(VxCoreContextHandle context,
                                                 const char *notebook_id,
                                                 VxCoreSyncCancellation *token) {
  if (!context || !notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    if (!ctx->sync_manager) {
      ctx->last_error = "Sync manager not initialized";
      return VXCORE_ERR_UNKNOWN;
    }

    vxcore::SyncCancellationPtr ptr = (token != nullptr) ? token->ptr : nullptr;
    return ctx->sync_manager->NetworkPhaseOnly(notebook_id, std::move(ptr));
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error running network-phase sync";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_sync_get_status(VxCoreContextHandle context, const char *notebook_id,
                                              char **out_status_json) {
  if (!context || !notebook_id || !out_status_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    if (!ctx->sync_manager) {
      ctx->last_error = "Sync manager not initialized";
      return VXCORE_ERR_UNKNOWN;
    }

    vxcore::SyncState state;
    std::vector<vxcore::SyncFileInfo> files;
    VxCoreError err = ctx->sync_manager->GetSyncStatus(notebook_id, state, files);
    if (err != VXCORE_OK) {
      return err;
    }

    nlohmann::json j;
    j["state"] = SyncStateToString(state);
    j["files"] = nlohmann::json::array();
    for (const auto &f : files) {
      nlohmann::json fj;
      fj[vxcore::kJsonKeyPath] = f.path;
      fj["status"] = SyncFileStatusToString(f.status);
      j["files"].push_back(fj);
    }

    char *json_copy = vxcore_strdup(j.dump().c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_status_json = json_copy;
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error getting sync status";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_sync_get_conflicts(VxCoreContextHandle context,
                                                 const char *notebook_id,
                                                 char **out_conflicts_json) {
  if (!context || !notebook_id || !out_conflicts_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    if (!ctx->sync_manager) {
      ctx->last_error = "Sync manager not initialized";
      return VXCORE_ERR_UNKNOWN;
    }

    std::vector<vxcore::SyncConflictInfo> conflicts;
    VxCoreError err = ctx->sync_manager->GetConflicts(notebook_id, conflicts);
    if (err != VXCORE_OK) {
      return err;
    }

    nlohmann::json j;
    j["conflicts"] = nlohmann::json::array();
    for (const auto &c : conflicts) {
      nlohmann::json cj;
      cj[vxcore::kJsonKeyPath] = c.path;
      cj[vxcore::kJsonKeyLocalModifiedUtc] = c.local_modified_utc;
      cj[vxcore::kJsonKeyRemoteModifiedUtc] = c.remote_modified_utc;
      cj[vxcore::kJsonKeyIsBinary] = c.is_binary;
      j["conflicts"].push_back(cj);
    }

    char *json_copy = vxcore_strdup(j.dump().c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_conflicts_json = json_copy;
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error getting sync conflicts";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_sync_resolve_conflict(VxCoreContextHandle context,
                                                    const char *notebook_id, const char *path,
                                                    const char *resolution) {
  if (!context || !notebook_id || !path || !resolution) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    if (!ctx->sync_manager) {
      ctx->last_error = "Sync manager not initialized";
      return VXCORE_ERR_UNKNOWN;
    }

    vxcore::SyncConflictResolution res;
    std::string res_str(resolution);
    if (res_str == "keep_both") {
      res = vxcore::SyncConflictResolution::kKeepBoth;
    } else if (res_str == "keep_local") {
      res = vxcore::SyncConflictResolution::kKeepLocal;
    } else if (res_str == "keep_remote") {
      res = vxcore::SyncConflictResolution::kKeepRemote;
    } else {
      ctx->last_error = "Invalid resolution: " + res_str;
      return VXCORE_ERR_INVALID_PARAM;
    }

    return ctx->sync_manager->ResolveConflict(notebook_id, path, res);
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error resolving sync conflict";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_sync_set_credentials(VxCoreContextHandle context,
                                                   const char *notebook_id,
                                                   const char *credentials_json) {
  if (!context || !notebook_id || !credentials_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    if (!ctx->sync_manager) {
      ctx->last_error = "Sync manager not initialized";
      return VXCORE_ERR_UNKNOWN;
    }

    vxcore::SyncCredentials creds = ParseCredentials(credentials_json);
    // NOTE: never log the PAT value itself.
    // Wave 6.3 F4.4: route through UpdateCredentials by wrapping the
    // incoming creds in an InMemoryCredentialProvider. The backend reads
    // credentials via ICredentialProvider exclusively — there is no
    // SetCredentials path anymore.
    auto provider = std::make_shared<vxcore::InMemoryCredentialProvider>(std::move(creds));
    return ctx->sync_manager->UpdateCredentials(notebook_id, std::move(provider));
  } catch (const nlohmann::json::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error setting sync credentials";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_sync_is_ready(VxCoreContextHandle context, const char *notebook_id,
                                            int *out_ready) {
  if (!context || !notebook_id || !out_ready) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    // Task 7.4 (F3.6): route through SyncManager — the single source of truth
    // for "is sync configured & ready". The predicate (sync_enabled +
    // non-empty backend + non-empty remote_url) lives inside
    // SyncManager::IsReady and is mirrored here without modification. We
    // still return NOT_FOUND for missing notebooks (preserves pre-7.4 C ABI
    // semantics) rather than silently reporting not-ready.
    if (!ctx->sync_manager) {
      ctx->last_error = "Sync manager not initialized";
      *out_ready = 0;
      return VXCORE_ERR_UNKNOWN;
    }
    auto *nb = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!nb) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }
    *out_ready = ctx->sync_manager->IsReady(notebook_id) ? 1 : 0;
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    *out_ready = 0;
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error checking sync readiness";
    *out_ready = 0;
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_sync_is_registered(VxCoreContextHandle context,
                                                 const char *notebook_id,
                                                 int *out_registered) {
  if (!context || !notebook_id || !out_registered) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    // Wave 14: lightweight runtime-registration predicate. Routes through
    // SyncManager::IsRegistered (state_mutex_ only, never touches backend
    // op_mutex_). The previous "is registered?" code path went through
    // vxcore_sync_get_status -> SyncManager::GetSyncStatus ->
    // GitSyncBackend::GetStatus which acquired the per-backend op_mutex_
    // blockingly, racing with worker-thread StageAndCommit/FetchRebasePush
    // and producing persistent VXCORE_ERR_SYNC_IN_PROGRESS on every UI-
    // driven check. This new entry point is safe to spin from the GUI
    // thread at arbitrary frequency.
    if (!ctx->sync_manager) {
      *out_registered = 0;
      return VXCORE_OK;
    }
    *out_registered = ctx->sync_manager->IsRegistered(notebook_id) ? 1 : 0;
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    *out_registered = 0;
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error checking sync registration";
    *out_registered = 0;
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_sync_get_last_sync_utc(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     int64_t *out_utc_millis) {
  if (!context || !notebook_id || !out_utc_millis) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    // Task 7.4 (F3.6): route through SyncManager. LastSyncTime delegates to
    // Notebook::GetLastSyncUtc internally — same metadata.db read as before,
    // same units (ms since Unix epoch, UTC). Missing notebook → 0 +
    // NOT_FOUND, mirroring the previous behaviour exactly.
    if (!ctx->sync_manager) {
      ctx->last_error = "Sync manager not initialized";
      *out_utc_millis = 0;
      return VXCORE_ERR_UNKNOWN;
    }
    auto *nb = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!nb) {
      ctx->last_error = "Notebook not found";
      *out_utc_millis = 0;
      return VXCORE_ERR_NOT_FOUND;
    }
    *out_utc_millis = ctx->sync_manager->LastSyncTime(notebook_id);
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    *out_utc_millis = 0;
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error reading last sync timestamp";
    *out_utc_millis = 0;
    return VXCORE_ERR_UNKNOWN;
  }
}
