#include "sync_types.h"

#include <nlohmann/json.hpp>

#include "sync/sync_json_keys.h"

namespace vxcore {

SyncConfig SyncConfig::FromJson(const nlohmann::json &json) {
  SyncConfig config;
  if (json.contains(kJsonKeyBackend) && json[kJsonKeyBackend].is_string())
    config.backend = json[kJsonKeyBackend].get<std::string>();
  if (json.contains(kJsonKeyRemoteUrl) && json[kJsonKeyRemoteUrl].is_string())
    config.remote_url = json[kJsonKeyRemoteUrl].get<std::string>();
  if (json.contains(kJsonKeyAutoSyncEnabled) && json[kJsonKeyAutoSyncEnabled].is_boolean())
    config.auto_sync_enabled = json[kJsonKeyAutoSyncEnabled].get<bool>();
  // NOTE (F3.3): the legacy `"enabled"` key is silently ignored for backward
  // compat with on-disk JSON written before the field was dropped. Whether
  // sync is "on" for a notebook is encoded by SyncManager registration.
  if (json.contains(kJsonKeyExcludePaths) && json[kJsonKeyExcludePaths].is_array()) {
    config.exclude_paths.clear();
    for (const auto &p : json[kJsonKeyExcludePaths]) {
      if (p.is_string()) config.exclude_paths.push_back(p.get<std::string>());
    }
  }
  if (json.contains(kJsonKeyBackendOptions) && json[kJsonKeyBackendOptions].is_object())
    config.backend_options = json[kJsonKeyBackendOptions];
  if (json.contains(kJsonKeyAutoCommitMerges) && json[kJsonKeyAutoCommitMerges].is_boolean())
    config.auto_commit_merges = json[kJsonKeyAutoCommitMerges].get<bool>();
  return config;
}

nlohmann::json SyncConfig::ToJson() const {
  nlohmann::json j;
  j[kJsonKeyBackend] = backend;
  j[kJsonKeyRemoteUrl] = remote_url;
  j[kJsonKeyAutoSyncEnabled] = auto_sync_enabled;
  j[kJsonKeyExcludePaths] = exclude_paths;
  j[kJsonKeyAutoCommitMerges] = auto_commit_merges;
  if (!backend_options.is_null() && !backend_options.empty())
    j[kJsonKeyBackendOptions] = backend_options;
  return j;
}

}  // namespace vxcore
