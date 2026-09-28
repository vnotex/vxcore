#ifndef VXCORE_SYNC_JIANGUOYUN_STATE_H
#define VXCORE_SYNC_JIANGUOYUN_STATE_H

#include <cstdint>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "sync/sync_cancellation.h"
#include "sync/sync_types.h"
#include "vxcore/notebook_json_keys.h"

namespace vxcore {
namespace jianguoyun {

inline constexpr const char *kFormat = "vnote-managed-sync";
inline constexpr int kFormatVersion = 1;
inline constexpr const char *kHeadPath = ".vnote-sync/head.json";
inline constexpr const char *kPrivatePath = "vx_notebook/vx_sync/jianguoyun";
inline constexpr const char *kConfigPath = "vx_notebook/config.json";
inline constexpr const char *kEncryptionPath = "vx_notebook/encryption.vne";
inline constexpr uint64_t kChunkBytes = 16ULL * 1024 * 1024;
inline constexpr size_t kManifestBytes = 128ULL * 1024 * 1024;
inline constexpr size_t kMaxEntries = 250000;
inline constexpr size_t kMaxDepth = 256;
inline constexpr size_t kMaxAncestry = 4096;

// These names are the version-one wire and recovery schema, not provider ETag state.
namespace key {
inline constexpr const char *kFormat = "format";
inline constexpr const char *kFormatVersion = "formatVersion";
inline constexpr const char *kRepositoryId = "repositoryId";
inline constexpr const char *kGeneration = "generation";
inline constexpr const char *kCommitHash = "commitHash";
inline constexpr const char *kOperationId = "operationId";
inline constexpr const char *kEntries = "entries";
inline constexpr const char *kRevision = "revision";
inline constexpr const char *kSize = "size";
inline constexpr const char *kSha256 = "sha256";
inline constexpr const char *kChunks = "chunks";
}  // namespace key

struct Chunk {
  std::string sha256;
  uint64_t size = 0;
};
struct Entry {
  // absent is an in-memory lookup result only; on disk deletions are tombstones.
  std::string kind = "absent";
  std::string revision;
  uint64_t size = 0;
  std::string sha256;
  std::vector<Chunk> chunks;
};
using Tree = std::map<std::string, Entry>;
struct Head {
  std::string repository_id;
  std::string notebook_id;
  uint64_t generation = 0;
  std::string commit_hash;
};
struct Commit {
  Head identity;
  std::string operation_id;
  std::string parent;
  Tree entries;
};
struct Version {
  Entry entry;
  std::string snapshot;
  std::string raw_sha256;
  int64_t modified_utc = 0;
  bool binary = false;
};
using Versions = std::map<std::string, Version>;
struct Conflict {
  Version local;
  Version remote;
  bool can_keep_both = false;
  std::string resolution;
};
struct Upload {
  std::string canonical;
  std::string staging;
  std::string snapshot;
  std::string sha256;
  uint64_t offset = 0;
  uint64_t size = 0;
  // prepared -> putStarted -> stagedUploaded -> moveStarted -> publishedVerified.
  std::string stage = "prepared";
};
struct ApplyOperation {
  Version expected;
  Version desired;
  std::string backup;
  std::string backup_sha256;
  // prepared -> installing -> localConfirmed -> baselineConfirmed.
  std::string stage = "prepared";
};
struct Pending {
  std::string operation_id;
  std::optional<Head> base_head;
  std::string base_token;
  std::optional<Head> intended_head;
  std::string head_snapshot;
  std::string head_staging;
  // prepared, publishing, published, applying. The head GET is always paired with its token.
  std::string phase = "prepared";
  bool fresh_bootstrap = false;
  Versions local;
  Versions target;
  std::map<std::string, Upload> uploads;
  std::map<std::string, ApplyOperation> apply;
};
struct RecoveryStatus {
  bool any_conflict = false;
  bool any_pending = false;
  bool encryption_conflict = false;
  bool encryption_pending = false;
  bool replaces_encryption = false;
};

nlohmann::json ParseRecord(const std::string &bytes, size_t limit = kManifestBytes);
bool IsHash(const std::string &value);
void ValidatePath(const std::string &path);
void ValidateTree(const Tree &tree);
bool Present(const Entry &entry);
bool Same(const Entry &a, const Entry &b);
bool SameRevision(const Entry &a, const Entry &b);
Entry Lookup(const Tree &tree, const std::string &path);
Version Lookup(const Versions &tree, const std::string &path);
Tree Entries(const Versions &versions);
nlohmann::json EncodeHead(const Head &head);
Head DecodeHead(const nlohmann::json &json);
nlohmann::json EncodeEntry(const Entry &entry);
Entry DecodeEntry(const nlohmann::json &json, bool allow_absent = false);
std::string EncodeCommit(const Commit &commit);
Commit DecodeCommit(const std::string &bytes, const std::string &expected_hash);
std::string ObjectPath(const std::string &hash);
std::string CommitPath(const std::string &hash);
std::string StagingPath(const std::string &operation_id);
std::vector<Chunk> HashChunks(const std::filesystem::path &file, const SyncCancellationPtr &token);

class State {
 public:
  std::filesystem::path root;
  std::filesystem::path directory;
  std::string notebook_id;
  std::string remote_url;
  std::string username_hash;
  std::string repository_id;
  std::optional<Head> head;
  Tree entries;
  std::map<std::string, Conflict> conflicts;
  std::optional<Pending> pending;
  SyncCancellationPtr cancellation;

  void Open(const std::filesystem::path &notebook_root, const std::string &id,
            const std::string &url, const std::string &username);
  void LoadDirectory(const std::filesystem::path &path, bool verify_payloads = true);
  void Save() const;
  void SavePending() const;
  void ClearPending();
  void RotateUsername(const std::string &hash);
  std::filesystem::path Resolve(const std::string &relative) const;
  std::string AllocateSnapshot();
  std::string SnapshotBytes(const std::string &bytes);
  std::string SnapshotFile(const std::filesystem::path &file);
  void VerifySnapshot(const std::string &name, const std::string &hash,
                      std::optional<uint64_t> size = std::nullopt, bool verify_hash = true) const;
  void CollectSnapshots() const;
  nlohmann::json Binding() const;
  void ValidateFiles() const;
};

// Offline, strict validation. Only encryption.vne controls the global key/lease barrier;
// an ordinary encrypted-note conflict still contributes to any_conflict/any_pending.
VxCoreError InspectRecovery(const std::filesystem::path &directory, RecoveryStatus &out);

}  // namespace jianguoyun
}  // namespace vxcore

#endif  // VXCORE_SYNC_JIANGUOYUN_STATE_H
