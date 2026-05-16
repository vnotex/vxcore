#include <nlohmann/json.hpp>

#include "api/api_utils.h"
#include "core/context.h"
#include "core/notebook.h"
#include "core/notebook_manager.h"
#include "sync/sync_manager.h"
#include "sync/sync_types.h"
#include "utils/logger.h"
#include "vxcore/vxcore.h"

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

VXCORE_API VxCoreError vxcore_sync_enable(VxCoreContextHandle context, const char *notebook_id,
                                          const char *config_json) {
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
    config.enabled = true;

    return ctx->sync_manager->EnableSync(notebook_id, config);
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
      fj["path"] = f.path;
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
      cj["path"] = c.path;
      cj["localModifiedUtc"] = c.local_modified_utc;
      cj["remoteModifiedUtc"] = c.remote_modified_utc;
      cj["isBinary"] = c.is_binary;
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

// Parse credentials_json into a vxcore::SyncCredentials. All fields optional;
// missing or non-string fields become empty strings. Throws nlohmann::json::exception
// on malformed JSON (caught by callers).
static vxcore::SyncCredentials ParseCredentials(const char *credentials_json) {
  vxcore::SyncCredentials creds;
  auto j = nlohmann::json::parse(credentials_json);
  if (j.contains("pat") && j["pat"].is_string()) {
    creds.personal_access_token = j["pat"].get<std::string>();
  }
  if (j.contains("authorName") && j["authorName"].is_string()) {
    creds.author_name = j["authorName"].get<std::string>();
  }
  if (j.contains("authorEmail") && j["authorEmail"].is_string()) {
    creds.author_email = j["authorEmail"].get<std::string>();
  }
  if (j.contains("extra") && j["extra"].is_object()) {
    creds.extra = j["extra"];
  }
  return creds;
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
    return ctx->sync_manager->SetCredentials(notebook_id, creds);
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
    auto *nb = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!nb) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    const auto &cfg = nb->GetConfig();
    *out_ready =
        (cfg.sync_enabled && !cfg.sync_backend.empty() && !cfg.sync_remote_url.empty()) ? 1 : 0;
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error checking sync readiness";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_sync_enable_with_credentials(VxCoreContextHandle context,
                                                           const char *notebook_id,
                                                           const char *config_json,
                                                           const char *credentials_json) {
  if (!context || !notebook_id || !config_json || !credentials_json) {
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
    config.enabled = true;

    vxcore::SyncCredentials creds = ParseCredentials(credentials_json);
    // NOTE: never log the PAT value itself.
    return ctx->sync_manager->EnableSync(notebook_id, config, creds);
  } catch (const nlohmann::json::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error enabling sync with credentials";
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
    auto *nb = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!nb) {
      ctx->last_error = "Notebook not found";
      *out_utc_millis = 0;
      return VXCORE_ERR_NOT_FOUND;
    }
    *out_utc_millis = nb->GetLastSyncUtc();
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
