#include "webdav_state.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "core/folder.h"
#include "core/notebook.h"
#include "sync/sync_json_keys.h"
#include "utils/file_utils.h"
#include "utils/string_utils.h"
#include "vxcore/notebook_json_keys.h"
#include "webdav_transport.h"

namespace vxcore {
namespace webdav {
namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace {

std::string Hex(const NotebookEncryption::Fingerprint &hash) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result(64, '0');
  for (size_t i = 0; i < hash.size(); ++i) {
    result[i * 2] = digits[hash[i] >> 4];
    result[i * 2 + 1] = digits[hash[i] & 15];
  }
  return result;
}

bool IsHash(const std::string &text) {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::string String(const Json &j, const char *key, bool nullable = false) {
  Require(j.contains(key));
  if (nullable && j.at(key).is_null()) return {};
  Require(j.at(key).is_string());
  return j.at(key).get<std::string>();
}

Json Nullable(const std::string &s) { return s.empty() ? Json(nullptr) : Json(s); }

void HashField(const std::string &hash, bool nullable = true) {
  Require((nullable && hash.empty()) || IsHash(hash));
}

void PathField(const std::string &path, bool scratch = false) {
  Require(WebDavTransport::ValidateRelativePath(path) == VXCORE_OK);
  Require(scratch || !IsScratch(path));
  static const SyncConfig config;
  Require(!IsExcluded(path, config));
}

void SnapshotField(const std::string &name) {
  if (name.empty()) return;
  Check(WebDavTransport::ValidateRelativePath(name));
  const auto components = SplitPathComponents(name);
  Require(components.size() == 3 && components[0] == "snapshots" &&
          NotebookEncryption::IsCanonicalUuid(components[1]) &&
          NotebookEncryption::IsCanonicalUuid(components[2]));
}

void ExactKeys(const Json &j, const std::set<std::string> &required,
               const std::set<std::string> &optional = {}) {
  Require(j.is_object());
  for (const auto &key : required) Require(j.contains(key));
  for (auto it = j.begin(); it != j.end(); ++it)
    Require(required.count(it.key()) || optional.count(it.key()));
}

void ValidateBinding(const Json &j, const State &state, bool check_user) {
  Require(j.at("version").is_number_integer() && j.at("version") == 1);
  Require(String(j, kJsonKeyNotebookId) == state.notebook_id &&
          String(j, kJsonKeyRemoteUrl) == state.remote_url);
  const auto username = String(j, "usernameHash");
  HashField(username, false);
  Require(!check_user || username == state.username_hash);
}

Json Binding(const State &s) {
  return {{"version", 1},
          {kJsonKeyNotebookId, s.notebook_id},
          {kJsonKeyRemoteUrl, s.remote_url},
          {"usernameHash", s.username_hash}};
}

struct Identity {
  uint64_t device = 0;
  uint64_t file = 0;
  uint64_t size = 0;
  fs::file_time_type modified;
  bool operator==(const Identity &other) const {
    return device == other.device && file == other.file && size == other.size &&
           modified == other.modified;
  }
};

Identity Identify(const fs::path &path) {
  Identity result;
  Require(CheckReparsePoint(PathToUtf8(path)) == ReparseState::kNo, VXCORE_ERR_UNSUPPORTED);
#ifdef _WIN32
  HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  Require(handle != INVALID_HANDLE_VALUE, VXCORE_ERR_IO);
  BY_HANDLE_FILE_INFORMATION info{};
  const bool ok = GetFileInformationByHandle(handle, &info) != 0;
  CloseHandle(handle);
  Require(ok && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), VXCORE_ERR_IO);
  result.device = info.dwVolumeSerialNumber;
  result.file = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
#else
  struct stat info{};
  Require(lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode), VXCORE_ERR_IO);
  result.device = static_cast<uint64_t>(info.st_dev);
  result.file = static_cast<uint64_t>(info.st_ino);
#endif
  result.size = fs::file_size(path);
  result.modified = fs::last_write_time(path);
  return result;
}

void Reserve(const fs::path &path) {
#ifdef _WIN32
  int fd = -1;
  const auto error = _wsopen_s(&fd, path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                               _SH_DENYRW, _S_IREAD | _S_IWRITE);
  Require(error == 0 && fd >= 0, VXCORE_ERR_IO);
  Require(_close(fd) == 0, VXCORE_ERR_IO);
#else
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  Require(fd >= 0, VXCORE_ERR_IO);
  Require(close(fd) == 0, VXCORE_ERR_IO);
#endif
}

bool SafeName(const std::string &name) {
  return name.find('/') == std::string::npos &&
         WebDavTransport::ValidateRelativePath(name) == VXCORE_OK && !IsScratch(name);
}

void ValidateNotebook(const Json &j, const std::string &id) {
  Require(j.is_object() && j.contains(kJsonKeyId) && j.at(kJsonKeyId).is_string());
  for (const char *key :
       {kJsonKeyName, kJsonKeyDescription, kJsonKeyAssetsFolder, kJsonKeyRecycleBinFolder})
    Require(!j.contains(key) || j.at(key).is_string());
  Require(!j.contains(kJsonKeyMetadata) || j.at(kJsonKeyMetadata).is_object());
  Require(!j.contains(kJsonKeyTags) || j.at(kJsonKeyTags).is_array());
  Require(!j.contains(kJsonKeyIgnored) || j.at(kJsonKeyIgnored).is_array());
  const auto config = NotebookConfig::FromJson(j);
  Require(NotebookEncryption::IsCanonicalUuid(config.id) && (id.empty() || id == config.id));
  Require(!config.name.empty());
  Require(WebDavTransport::ValidateRelativePath(config.assets_folder) == VXCORE_OK &&
              WebDavTransport::ValidateRelativePath(config.recycle_bin_folder) == VXCORE_OK,
          VXCORE_ERR_UNSUPPORTED);
  static const SyncConfig defaults;
  Require(!IsExcluded(config.assets_folder, defaults) &&
              !IsExcluded(config.recycle_bin_folder, defaults) &&
              !IsScratch(config.assets_folder) && !IsScratch(config.recycle_bin_folder),
          VXCORE_ERR_UNSUPPORTED);
}

}  // namespace

void Require(bool condition, VxCoreError error) {
  if (!condition) throw Failure{error};
}
void Check(VxCoreError error) { Require(error == VXCORE_OK, error); }
void CheckCancelled(const SyncCancellationPtr &token) {
  Require(!token || !token->IsCancelled(), VXCORE_ERR_CANCELLED);
}
std::string NewId() {
  std::string id;
  Check(NotebookEncryption::GenerateIdentity(id));
  return id;
}
std::string HashBytes(const std::string &bytes) {
  NotebookEncryption::Fingerprint hash{};
  Check(NotebookEncryption::FingerprintBytes(bytes.data(), bytes.size(), hash));
  return Hex(hash);
}
std::string HashFile(const fs::path &path, const SyncCancellationPtr &token) {
  Require(sodium_init() >= 0, VXCORE_ERR_INVALID_STATE);
  std::ifstream input(path, std::ios::binary);
  Require(input.good(), VXCORE_ERR_IO);
  crypto_hash_sha256_state context;
  Require(crypto_hash_sha256_init(&context) == 0);
  std::array<char, 64 * 1024> bytes{};
  while (input) {
    CheckCancelled(token);
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount())
      Require(crypto_hash_sha256_update(&context,
                                        reinterpret_cast<const unsigned char *>(bytes.data()),
                                        static_cast<unsigned long long>(input.gcount())) == 0);
  }
  Require(input.eof() && !input.bad(), VXCORE_ERR_IO);
  NotebookEncryption::Fingerprint hash{};
  Require(crypto_hash_sha256_final(&context, hash.data()) == 0);
  return Hex(hash);
}
std::string ReadBytes(const fs::path &path, size_t limit) {
  const auto size = fs::file_size(path);
  Require(size <= limit, VXCORE_ERR_INVALID_STATE);
  std::ifstream input(path, std::ios::binary);
  Require(input.good(), VXCORE_ERR_IO);
  std::string result(static_cast<size_t>(size), '\0');
  input.read(result.data(), static_cast<std::streamsize>(result.size()));
  Require(input.gcount() == static_cast<std::streamsize>(result.size()) && !input.bad(),
          VXCORE_ERR_IO);
  Require(input.peek() == std::char_traits<char>::eof(), VXCORE_ERR_SYNC_IN_PROGRESS);
  return result;
}
bool IsScratch(const std::string &path) {
  for (const auto &component : SplitPathComponents(path))
    if (component.compare(0, std::char_traits<char>::length(kScratchPrefix), kScratchPrefix) == 0)
      return true;
  return false;
}
bool IsMetadata(const std::string &path) {
  return path == kConfigPath ||
         (path.compare(0, 21, "vx_notebook/contents/") == 0 && PathFilename(path) == "vx.json");
}
bool IsEncrypted(const std::string &path) {
  auto extension = PathToUtf8(PathFromUtf8(path).extension());
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension == ".vne";
}
bool IsExcluded(const std::string &path, const SyncConfig &config) {
  for (const auto &part : SplitPathComponents(path))
    if (part == ".git" || (part.size() >= 5 && part.compare(part.size() - 5, 5, ".vswp") == 0))
      return true;
  for (const char *prefix : {"vx_notebook/vx_sync", "vx_notebook/vx_transfer"}) {
    const std::string p(prefix);
    if (path == p || path.compare(0, p.size() + 1, p + "/") == 0) return true;
  }
  std::string candidate = path;
  for (;;) {
    if (MatchesPatterns(candidate, config.exclude_paths) ||
        MatchesPatterns(candidate + "/", config.exclude_paths) ||
        MatchesPatterns(PathFilename(candidate), config.exclude_paths))
      return true;
    const auto slash = candidate.rfind('/');
    if (slash == std::string::npos) break;
    candidate.resize(slash);
  }
  return false;
}
Json NotebookJson(const fs::path &file, const std::string &notebook_id) {
  auto json = Json::parse(ReadBytes(file));
  ValidateNotebook(json, notebook_id);
  return json;
}
std::string ProjectConfig(const Json &json) {
  auto projected = json;
  for (const char *key :
       {kJsonKeySyncEnabled, kJsonKeySyncBackend, kJsonKeySyncRemoteUrl, kJsonKeyAutoSyncEnabled})
    projected.erase(key);
  return projected.dump();
}
std::string RestoreRouting(const std::string &projected, const Json &current) {
  auto json = Json::parse(projected);
  for (const char *key :
       {kJsonKeySyncEnabled, kJsonKeySyncBackend, kJsonKeySyncRemoteUrl, kJsonKeyAutoSyncEnabled}) {
    json.erase(key);
    if (current.contains(key)) json[key] = current.at(key);
  }
  return json.dump();
}
void ValidateMetadata(const std::string &path, const fs::path &file, const std::string &notebook_id,
                      std::vector<std::string> *ids) {
  if (path == kConfigPath) {
    NotebookJson(file, notebook_id);
    return;
  }
  if (path == "vx_notebook/encryption.vne") {
    NotebookEncryption::KeyEnvelope envelope;
    Check(NotebookEncryption::ReadKeyEnvelope(file, envelope));
    Require(envelope.notebook_id == notebook_id, VXCORE_ERR_ENCRYPTION_FORMAT);
    return;
  }
  if (!IsMetadata(path)) return;
  const auto json = Json::parse(ReadBytes(file));
  Require(json.is_object() && json.contains(kJsonKeyFiles) && json.at(kJsonKeyFiles).is_array() &&
          json.contains(kJsonKeyFolders) && json.at(kJsonKeyFolders).is_array());
  const auto folder = FolderConfig::FromJson(json);
  Require(NotebookEncryption::IsCanonicalUuid(folder.id) && !folder.name.empty());
  const auto parent_name = PathToUtf8(PathFromUtf8(path).parent_path().filename());
  Require(path == "vx_notebook/contents/vx.json"
              ? folder.name == "."
              : SafeName(folder.name) && folder.name == parent_name);
  std::set<std::string> seen_ids{folder.id}, names;
  for (const auto &entry : folder.files) {
    Require(NotebookEncryption::IsCanonicalUuid(entry.id) && SafeName(entry.name) &&
            seen_ids.insert(entry.id).second && names.insert(entry.name).second);
    const auto protection = entry.CheckProtectionMetadata();
    Require(protection == VXCORE_OK || protection == VXCORE_ERR_ENCRYPTION_LOCKED, protection);
  }
  for (const auto &name : folder.folders) Require(SafeName(name) && names.insert(name).second);
  if (ids) ids->insert(ids->end(), seen_ids.begin(), seen_ids.end());
}
bool Same(const Version &a, const Version &b) {
  return a.kind == b.kind && (a.kind != "file" || a.sha256 == b.sha256);
}
Version Lookup(const Tree &tree, const std::string &path) {
  const auto it = tree.find(path);
  return it == tree.end() ? Version{} : it->second;
}

void EnsureDirectory(const fs::path &root, const std::string &relative) {
  fs::path resolved;
  Check(WebDavTransport::ResolveLocalPath(root, relative, resolved));
  std::error_code error;
  fs::create_directories(resolved, error);
  Require(!error && fs::is_directory(resolved), VXCORE_ERR_IO);
  Check(WebDavTransport::ResolveLocalPath(root, relative, resolved));
}

void CopyAtomic(const fs::path &source, const fs::path &destination,
                const SyncCancellationPtr &token) {
  const auto before = Identify(source);
  std::ifstream input(source, std::ios::binary);
  Require(input.good(), VXCORE_ERR_IO);
  AtomicFileWriter writer(destination);
  Check(writer.Open());
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    CheckCancelled(token);
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count) Check(writer.Write(buffer.data(), static_cast<size_t>(count)));
  }
  Require(input.eof() && !input.bad(), VXCORE_ERR_IO);
  Require(before == Identify(source), VXCORE_ERR_SYNC_IN_PROGRESS);
  CheckCancelled(token);
  Check(writer.Commit());
}

fs::path State::Resolve(const std::string &relative) const {
  fs::path result;
  Check(WebDavTransport::ResolveLocalPath(directory, relative, result));
  return result;
}
void State::Open(const fs::path &notebook_root, const std::string &id, const std::string &url,
                 const std::string &username, const std::string &storage_relative) {
  root = notebook_root;
  notebook_id = id;
  remote_url = url;
  username_hash = username;
  EnsureDirectory(root, storage_relative);
  Check(WebDavTransport::ResolveLocalPath(root, storage_relative, directory));
  entries.clear();
  conflicts.clear();
  operations.clear();
  operation_id.clear();
  const auto state_path = Resolve("state.json");
  existed = fs::exists(state_path);
  if (existed) {
    auto j = Json::parse(ReadBytes(state_path, 128 * 1024 * 1024));
    ExactKeys(j, {"version", kJsonKeyNotebookId, kJsonKeyRemoteUrl, "usernameHash", "entries",
                  "conflicts"});
    ValidateBinding(j, *this, false);
    username_hash = String(j, "usernameHash");
    Require(j.at("entries").is_object() && j.at("conflicts").is_object());
    for (auto it = j["entries"].begin(); it != j["entries"].end(); ++it) {
      CheckCancelled(cancellation);
      PathField(it.key());
      Version v;
      v.kind = String(it.value(), "kind");
      Require(v.kind == "file" || v.kind == "collection");
      if (v.kind == "file") {
        ExactKeys(it.value(), {"kind", "sha256", "etag"});
        v.sha256 = String(it.value(), "sha256");
        HashField(v.sha256, false);
        v.etag = String(it.value(), "etag");
        Require(WebDavTransport::IsStrongEtag(v.etag));
      } else
        ExactKeys(it.value(), {"kind"});
      entries.emplace(it.key(), std::move(v));
    }
    for (auto it = j["conflicts"].begin(); it != j["conflicts"].end(); ++it) {
      CheckCancelled(cancellation);
      PathField(it.key());
      const auto &c = it.value();
      ExactKeys(c,
                {"localSha256", "remoteSha256", "remoteEtag", "localSnapshot", "remoteSnapshot",
                 kJsonKeyIsBinary, kJsonKeyCanKeepBoth, kJsonKeyLocalModifiedUtc,
                 kJsonKeyRemoteModifiedUtc},
                {"resolution"});
      Conflict conflict;
      conflict.local.sha256 = String(c, "localSha256", true);
      conflict.remote.sha256 = String(c, "remoteSha256", true);
      HashField(conflict.local.sha256);
      HashField(conflict.remote.sha256);
      conflict.local.kind = conflict.local.sha256.empty() ? "absent" : "file";
      conflict.remote.kind = conflict.remote.sha256.empty() ? "absent" : "file";
      conflict.remote.etag = String(c, "remoteEtag", true);
      Require(conflict.remote.kind == "file" ? WebDavTransport::IsStrongEtag(conflict.remote.etag)
                                             : conflict.remote.etag.empty());
      conflict.local.snapshot = String(c, "localSnapshot", true);
      conflict.remote.snapshot = String(c, "remoteSnapshot", true);
      SnapshotField(conflict.local.snapshot);
      SnapshotField(conflict.remote.snapshot);
      Require((conflict.local.kind == "file") == !conflict.local.snapshot.empty() &&
              (conflict.remote.kind == "file") == !conflict.remote.snapshot.empty());
      Require(c.at(kJsonKeyIsBinary).is_boolean() && c.at(kJsonKeyCanKeepBoth).is_boolean() &&
              c.at(kJsonKeyLocalModifiedUtc).is_number_integer() &&
              c.at(kJsonKeyRemoteModifiedUtc).is_number_integer());
      conflict.local.binary = c.at(kJsonKeyIsBinary).get<bool>();
      conflict.can_keep_both = c.at(kJsonKeyCanKeepBoth).get<bool>();
      Require(!(IsMetadata(it.key()) || IsEncrypted(it.key())) || !conflict.can_keep_both);
      conflict.local.modified_utc = c.at(kJsonKeyLocalModifiedUtc).get<int64_t>();
      conflict.remote.modified_utc = c.at(kJsonKeyRemoteModifiedUtc).get<int64_t>();
      if (c.contains("resolution")) {
        conflict.resolution = String(c, "resolution");
        Require(conflict.resolution == "keep_local" || conflict.resolution == "keep_remote" ||
                (conflict.resolution == "keep_both" && conflict.can_keep_both));
      }
      VerifySnapshot(conflict.local.snapshot, conflict.local.sha256);
      VerifySnapshot(conflict.remote.snapshot, conflict.remote.sha256);
      conflicts.emplace(it.key(), std::move(conflict));
    }
  }
  const auto pending_path = Resolve("pending.json");
  if (!fs::exists(pending_path)) return;
  Require(existed);
  const auto j = Json::parse(ReadBytes(pending_path, 128 * 1024 * 1024));
  ExactKeys(j, {"version", kJsonKeyNotebookId, kJsonKeyRemoteUrl, "usernameHash", "operationId",
                "operations"});
  ValidateBinding(j, *this, true);
  operation_id = String(j, "operationId");
  Require(NotebookEncryption::IsCanonicalUuid(operation_id) && j.at("operations").is_array());
  Require(j.at("operations").size() <= 500000);
  std::set<std::pair<std::string, std::string>> unique;
  for (const auto &o : j.at("operations")) {
    CheckCancelled(cancellation);
    ExactKeys(o, {"path", "kind", "action", "expectedLocalKind", "expectedRemoteKind",
                  "expectedLocalSha256", "expectedRemoteEtag", "oldSha256", "newSha256",
                  "sourceSnapshot", "previousSnapshot", "scratchUrl", "stage"});
    Operation op;
    op.path = String(o, "path");
    PathField(op.path, true);
    op.kind = String(o, "kind");
    Require(op.kind == "file" || op.kind == "collection");
    op.action = String(o, "action");
    Require(op.action == "upsertLocal" || op.action == "upsertRemote" ||
            op.action == "deleteLocal" || op.action == "deleteRemote" ||
            op.action == "mkdirLocal" || op.action == "mkdirRemote" ||
            op.action == "removeLocalCollection");
    Require(unique.emplace(op.path, op.action).second);
    op.expected_local_kind = String(o, "expectedLocalKind");
    op.expected_remote_kind = String(o, "expectedRemoteKind");
    for (const auto &kind : {op.expected_local_kind, op.expected_remote_kind})
      Require(kind == "absent" || kind == "file" || kind == "collection");
    op.expected_local_sha256 = String(o, "expectedLocalSha256", true);
    op.expected_remote_etag = String(o, "expectedRemoteEtag", true);
    op.old_sha256 = String(o, "oldSha256", true);
    op.new_sha256 = String(o, "newSha256", true);
    HashField(op.expected_local_sha256);
    HashField(op.old_sha256);
    HashField(op.new_sha256);
    Require((op.expected_local_kind == "file") == !op.expected_local_sha256.empty());
    Require(op.expected_remote_kind == "file"
                ? WebDavTransport::IsStrongEtag(op.expected_remote_etag)
                : op.expected_remote_etag.empty());
    op.source_snapshot = String(o, "sourceSnapshot", true);
    op.previous_snapshot = String(o, "previousSnapshot", true);
    SnapshotField(op.source_snapshot);
    SnapshotField(op.previous_snapshot);
    op.scratch_url = String(o, "scratchUrl", true);
    if (!op.scratch_url.empty()) {
      Check(WebDavTransport::ValidateRelativePath(op.scratch_url));
      Require(op.scratch_url.find('/') == std::string::npos && IsScratch(op.scratch_url) &&
              NotebookEncryption::IsCanonicalUuid(
                  op.scratch_url.substr(std::char_traits<char>::length(kScratchPrefix))));
    }
    op.stage = String(o, "stage");
    Require(op.stage == "prepared" || op.stage == "remoteConfirmed" ||
            op.stage == "localConfirmed" || op.stage == "baselineConfirmed");
    const bool upsert = op.action == "upsertLocal" || op.action == "upsertRemote";
    Require(upsert ? op.kind == "file" && !op.new_sha256.empty() && !op.source_snapshot.empty()
                   : op.new_sha256.empty() && op.source_snapshot.empty());
    if (!op.source_snapshot.empty()) VerifySnapshot(op.source_snapshot, op.new_sha256);
    if (!op.previous_snapshot.empty()) VerifySnapshot(op.previous_snapshot, op.old_sha256);
    Require(op.old_sha256.empty() == op.previous_snapshot.empty());
    const bool collection_action = op.action == "mkdirLocal" || op.action == "mkdirRemote" ||
                                   op.action == "removeLocalCollection";
    Require((op.kind == "collection") == collection_action);
    if (collection_action) {
      Require(op.expected_local_kind != "file" && op.expected_remote_kind != "file" &&
              op.old_sha256.empty() && op.scratch_url.empty());
    } else {
      Require(op.expected_local_kind != "collection" && op.expected_remote_kind != "collection");
      if ((op.action == "upsertRemote" || op.action == "deleteRemote") &&
          op.expected_remote_kind == "file")
        Require(!op.previous_snapshot.empty());
      Require(op.scratch_url.empty() || op.action == "upsertRemote");
      if (op.action == "upsertRemote" && !IsScratch(op.path)) Require(!op.scratch_url.empty());
    }
    operations.push_back(std::move(op));
  }
}

void State::Save() const {
  auto j = Binding(*this);
  j["entries"] = Json::object();
  j["conflicts"] = Json::object();
  for (const auto &entry : entries) {
    Json value = {{"kind", entry.second.kind}};
    if (entry.second.kind == "file") {
      value["sha256"] = entry.second.sha256;
      value["etag"] = entry.second.etag;
    }
    j["entries"][entry.first] = std::move(value);
  }
  for (const auto &entry : conflicts) {
    const auto &c = entry.second;
    Json value = {{"localSha256", Nullable(c.local.sha256)},
                  {"remoteSha256", Nullable(c.remote.sha256)},
                  {"remoteEtag", Nullable(c.remote.etag)},
                  {"localSnapshot", Nullable(c.local.snapshot)},
                  {"remoteSnapshot", Nullable(c.remote.snapshot)},
                  {kJsonKeyIsBinary, c.local.binary || c.remote.binary},
                  {kJsonKeyCanKeepBoth, c.can_keep_both},
                  {kJsonKeyLocalModifiedUtc, c.local.modified_utc},
                  {kJsonKeyRemoteModifiedUtc, c.remote.modified_utc}};
    if (!c.resolution.empty()) value["resolution"] = c.resolution;
    j["conflicts"][entry.first] = std::move(value);
  }
  Check(WriteFileAtomic(Resolve("state.json"), j.dump()));
}
void State::SavePending() const {
  Require(NotebookEncryption::IsCanonicalUuid(operation_id));
  auto j = Binding(*this);
  j["operationId"] = operation_id;
  j["operations"] = Json::array();
  for (const auto &o : operations)
    j["operations"].push_back({{"path", o.path},
                               {"kind", o.kind},
                               {"action", o.action},
                               {"expectedLocalKind", o.expected_local_kind},
                               {"expectedRemoteKind", o.expected_remote_kind},
                               {"expectedLocalSha256", Nullable(o.expected_local_sha256)},
                               {"expectedRemoteEtag", Nullable(o.expected_remote_etag)},
                               {"oldSha256", Nullable(o.old_sha256)},
                               {"newSha256", Nullable(o.new_sha256)},
                               {"sourceSnapshot", Nullable(o.source_snapshot)},
                               {"previousSnapshot", Nullable(o.previous_snapshot)},
                               {"scratchUrl", Nullable(o.scratch_url)},
                               {"stage", o.stage}});
  Check(WriteFileAtomic(Resolve("pending.json"), j.dump()));
}
void State::ClearPending() {
  std::error_code error;
  fs::remove(Resolve("pending.json"), error);
  Require(!error, VXCORE_ERR_IO);
  operations.clear();
  operation_id.clear();
}
void State::RotateUsername(const std::string &hash) {
  if (hash == username_hash) return;
  // Two binding files cannot be atomically replaced together. Do not rotate while recovery
  // owns a journal; complete it with authenticated credentials before changing its binding.
  Require(operations.empty(), VXCORE_ERR_SYNC_IN_PROGRESS);
  const auto previous = username_hash;
  username_hash = hash;
  try {
    Save();
  } catch (...) {
    username_hash = previous;
    throw;
  }
}
void State::Begin() {
  if (operation_id.empty()) operation_id = NewId();
}
std::string State::AllocateSnapshot() {
  Begin();
  const auto folder = "snapshots/" + operation_id;
  EnsureDirectory(directory, folder);
  const auto name = folder + "/" + NewId();
  Reserve(Resolve(name));
  transient_snapshots.push_back(name);
  return name;
}
std::string State::SnapshotBytes(const std::string &bytes) {
  const auto name = AllocateSnapshot();
  Check(WriteFileAtomic(Resolve(name), bytes));
  return name;
}
std::string State::SnapshotFile(const fs::path &source, const SyncCancellationPtr &token) {
  const auto name = AllocateSnapshot();
  CopyAtomic(source, Resolve(name), token);
  return name;
}
void State::VerifySnapshot(const std::string &name, const std::string &hash) const {
  if (name.empty()) {
    Require(hash.empty());
    return;
  }
  SnapshotField(name);
  HashField(hash, false);
  Require(HashFile(Resolve(name), cancellation) == hash);
}
void State::RemoveSnapshot(const std::string &name) const {
  if (name.empty()) return;
  SnapshotField(name);
  std::error_code error;
  fs::remove(Resolve(name), error);
  Require(!error, VXCORE_ERR_IO);
}
void State::RemoveUnreferencedSnapshots(const std::vector<Operation> &completed) const {
  std::set<std::string> keep;
  for (const auto &c : conflicts) {
    keep.insert(c.second.local.snapshot);
    keep.insert(c.second.remote.snapshot);
  }
  for (const auto &op : operations) {
    keep.insert(op.source_snapshot);
    keep.insert(op.previous_snapshot);
  }
  std::set<std::string> remove;
  for (const auto &op : completed) {
    remove.insert(op.source_snapshot);
    remove.insert(op.previous_snapshot);
  }
  for (const auto &name : remove)
    if (!name.empty() && !keep.count(name)) RemoveSnapshot(name);
}

void State::CleanupTransient() {
  std::vector<Operation> temporary;
  temporary.reserve(transient_snapshots.size());
  for (const auto &name : transient_snapshots) {
    Operation op;
    op.source_snapshot = name;
    temporary.push_back(std::move(op));
  }
  RemoveUnreferencedSnapshots(temporary);
  transient_snapshots.clear();
}

Version ReadLocal(const fs::path &root, const std::string &path, const std::string &notebook_id,
                  const SyncCancellationPtr &token, State *snapshots) {
  CheckCancelled(token);
  fs::path file;
  Check(WebDavTransport::ResolveLocalPath(root, path, file));
  Version version;
  std::error_code error;
  const auto status = fs::symlink_status(file, error);
  if (error == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found)
    return version;
  Require(!error, VXCORE_ERR_IO);
  Require(CheckReparsePoint(PathToUtf8(file)) == ReparseState::kNo, VXCORE_ERR_UNSUPPORTED);
  if (fs::is_directory(status)) {
    version.kind = "collection";
    return version;
  }
  Require(fs::is_regular_file(status), VXCORE_ERR_UNSUPPORTED);
  version.kind = "file";
  const auto before = Identify(file);
  version.raw_sha256 = HashFile(file, token);
  version.sha256 = version.raw_sha256;
  ValidateMetadata(path, file, notebook_id);
  if (path == kConfigPath) {
    const auto projected = ProjectConfig(NotebookJson(file, notebook_id));
    version.sha256 = HashBytes(projected);
    if (snapshots) version.snapshot = snapshots->SnapshotBytes(projected);
  } else if (snapshots)
    version.snapshot = snapshots->SnapshotFile(file, token);
  if (!version.snapshot.empty()) snapshots->VerifySnapshot(version.snapshot, version.sha256);
  Require(before == Identify(file), VXCORE_ERR_SYNC_IN_PROGRESS);
  Require(GetFilesystemTimes(PathToUtf8(file), nullptr, &version.modified_utc), VXCORE_ERR_IO);
  std::ifstream input(file, std::ios::binary);
  Require(input.good(), VXCORE_ERR_IO);
  std::array<char, 8192> sample{};
  input.read(sample.data(), static_cast<std::streamsize>(sample.size()));
  version.binary = IsEncrypted(path) || std::find(sample.begin(), sample.begin() + input.gcount(),
                                                  '\0') != sample.begin() + input.gcount();
  CheckCancelled(token);
  return version;
}

}  // namespace webdav
}  // namespace vxcore
