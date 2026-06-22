#ifndef VXCORE_SYNC_TYPES_H
#define VXCORE_SYNC_TYPES_H

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "vxcore/vxcore_types.h"

namespace vxcore {

enum class SyncState {
  kIdle,
  kStaging,
  kFetching,
  kAnalyzing,
  kMerging,
  kPushing,
  kError,
  kConflicted
};

enum class SyncFileStatus {
  kUnchanged,
  kModifiedLocal,
  kModifiedRemote,
  kConflicted,
  kAddedLocal,
  kAddedRemote,
  kDeletedLocal,
  kDeletedRemote
};

enum class SyncConflictResolution {
  kKeepBoth,
  kKeepLocal,
  kKeepRemote
};

struct SyncProgress {
  std::string message;
  float percentage = 0.0f;
  SyncState current_state = SyncState::kIdle;
};

struct SyncFileInfo {
  std::string path;
  SyncFileStatus status = SyncFileStatus::kUnchanged;
};

struct SyncConflictInfo {
  std::string path;
  int64_t local_modified_utc = 0;
  int64_t remote_modified_utc = 0;
  bool is_binary = false;
};

struct SyncConfig {
  // NOTE (F3.3): the legacy `enabled` flag was removed. Registration in
  // SyncManager already encodes "enabled" — query
  // SyncManager::IsSyncRegistered(notebook_id) instead. Old JSON containing
  // an `"enabled"` key is silently ignored by FromJson() for backward compat.
  std::string backend;
  std::string remote_url;
  bool auto_sync_enabled = true;
  std::vector<std::string> exclude_paths = {"*.vswp", "vx_notebook/vx_sync/"};
  nlohmann::json backend_options;
  bool auto_commit_merges = true;

  static VXCORE_API SyncConfig FromJson(const nlohmann::json &json);
  VXCORE_API nlohmann::json ToJson() const;
};

struct SyncCredentials {
  // When non-empty, HTTPS auth via libgit2 callback uses ("x-access-token", PAT).
  std::string personal_access_token;

  // Per-device commit author name. When empty, backend uses "VNote Sync" default.
  std::string author_name;

  // Per-device commit author email. When empty, backend uses "sync@vnote.local" default.
  std::string author_email;

  nlohmann::json extra;
};

using SyncProgressCallback = std::function<void(const SyncProgress &progress, void *userdata)>;

}  // namespace vxcore

#endif  // VXCORE_SYNC_TYPES_H
