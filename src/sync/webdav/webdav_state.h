#ifndef VXCORE_SYNC_WEBDAV_STATE_H
#define VXCORE_SYNC_WEBDAV_STATE_H

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "sync/sync_cancellation.h"
#include "sync/sync_types.h"

namespace vxcore {
namespace webdav {

inline constexpr const char *kConfigPath = "vx_notebook/config.json";
inline constexpr const char *kPrivatePath = "vx_notebook/vx_sync/webdav";
inline constexpr const char *kScratchPrefix = ".vnote-webdav-tmp-";

struct Failure {
  VxCoreError error;
};
void Require(bool condition, VxCoreError error = VXCORE_ERR_INVALID_STATE);
void Check(VxCoreError error);
void CheckCancelled(const SyncCancellationPtr &token);
std::string NewId();
std::string HashBytes(const std::string &bytes);
std::string HashFile(const std::filesystem::path &path, const SyncCancellationPtr &token = nullptr);
std::string ReadBytes(const std::filesystem::path &path, size_t limit = 16 * 1024 * 1024);
bool IsScratch(const std::string &path);
bool IsMetadata(const std::string &path);
bool IsEncrypted(const std::string &path);
bool IsExcluded(const std::string &path, const SyncConfig &config);
void ValidateMetadata(const std::string &path, const std::filesystem::path &file,
                      const std::string &notebook_id, std::vector<std::string> *ids = nullptr);
nlohmann::json NotebookJson(const std::filesystem::path &file, const std::string &notebook_id = {});
std::string ProjectConfig(const nlohmann::json &json);
std::string RestoreRouting(const std::string &projected, const nlohmann::json &current);

struct Version {
  std::string kind = "absent";
  std::string sha256;
  std::string etag;
  // Raw on-disk fingerprint, distinct from the projected config representation.
  std::string raw_sha256;
  std::string snapshot;
  int64_t modified_utc = 0;
  bool binary = false;
};
bool Same(const Version &a, const Version &b);
using Tree = std::map<std::string, Version>;
Version Lookup(const Tree &tree, const std::string &path);

struct Conflict {
  Version local;
  Version remote;
  bool can_keep_both = true;
  std::string resolution;
};

struct Operation {
  std::string path;
  std::string kind = "file";
  std::string action;
  std::string expected_local_kind = "absent";
  std::string expected_remote_kind = "absent";
  std::string expected_local_sha256;
  std::string expected_remote_etag;
  std::string old_sha256;
  std::string new_sha256;
  std::string source_snapshot;
  std::string previous_snapshot;
  // Decoded root-relative owned scratch name, never a journal-supplied absolute URL.
  std::string scratch_url;
  std::string stage = "prepared";
};

// No absolute paths, passwords or credential material occur in the persistent schema.
// All helpers throw Failure (or standard exceptions caught at the backend boundary).
class State {
 public:
  std::filesystem::path root;
  std::filesystem::path directory;
  std::string notebook_id;
  std::string remote_url;
  std::string username_hash;
  Tree entries;
  std::map<std::string, Conflict> conflicts;
  std::string operation_id;
  std::vector<Operation> operations;
  bool existed = false;
  std::vector<std::string> transient_snapshots;
  SyncCancellationPtr cancellation;

  void Open(const std::filesystem::path &notebook_root, const std::string &id,
            const std::string &url, const std::string &username,
            const std::string &storage_relative = kPrivatePath);
  void Save() const;
  void SavePending() const;
  void ClearPending();
  void RotateUsername(const std::string &hash);
  void Begin();
  std::filesystem::path Resolve(const std::string &relative) const;
  std::string AllocateSnapshot();
  std::string SnapshotBytes(const std::string &bytes);
  std::string SnapshotFile(const std::filesystem::path &source, const SyncCancellationPtr &token);
  void VerifySnapshot(const std::string &name, const std::string &hash) const;
  void RemoveSnapshot(const std::string &name) const;
  void RemoveUnreferencedSnapshots(const std::vector<Operation> &completed) const;
  void CleanupTransient();
};

Version ReadLocal(const std::filesystem::path &root, const std::string &path,
                  const std::string &notebook_id, const SyncCancellationPtr &token,
                  State *snapshots = nullptr);
void CopyAtomic(const std::filesystem::path &source, const std::filesystem::path &destination,
                const SyncCancellationPtr &token);
void EnsureDirectory(const std::filesystem::path &root, const std::string &relative);

}  // namespace webdav
}  // namespace vxcore

#endif  // VXCORE_SYNC_WEBDAV_STATE_H
