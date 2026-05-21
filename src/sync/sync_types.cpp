#include "sync_types.h"

#include <nlohmann/json.hpp>

namespace vxcore {

SyncConfig SyncConfig::FromJson(const nlohmann::json &json) {
  SyncConfig config;
  if (json.contains("backend") && json["backend"].is_string())
    config.backend = json["backend"].get<std::string>();
  if (json.contains("remoteUrl") && json["remoteUrl"].is_string())
    config.remote_url = json["remoteUrl"].get<std::string>();
  if (json.contains("intervalSeconds") && json["intervalSeconds"].is_number_integer())
    config.interval_seconds = json["intervalSeconds"].get<int>();
  // NOTE (F3.3): the legacy `"enabled"` key is silently ignored for backward
  // compat with on-disk JSON written before the field was dropped. Whether
  // sync is "on" for a notebook is encoded by SyncManager registration.
  if (json.contains("excludePaths") && json["excludePaths"].is_array()) {
    config.exclude_paths.clear();
    for (const auto &p : json["excludePaths"]) {
      if (p.is_string()) config.exclude_paths.push_back(p.get<std::string>());
    }
  }
  if (json.contains("backendOptions") && json["backendOptions"].is_object())
    config.backend_options = json["backendOptions"];
  if (json.contains("autoCommitMerges") && json["autoCommitMerges"].is_boolean())
    config.auto_commit_merges = json["autoCommitMerges"].get<bool>();
  return config;
}

nlohmann::json SyncConfig::ToJson() const {
  nlohmann::json j;
  j["backend"] = backend;
  j["remoteUrl"] = remote_url;
  j["intervalSeconds"] = interval_seconds;
  j["excludePaths"] = exclude_paths;
  j["autoCommitMerges"] = auto_commit_merges;
  if (!backend_options.is_null() && !backend_options.empty())
    j["backendOptions"] = backend_options;
  return j;
}

}  // namespace vxcore
