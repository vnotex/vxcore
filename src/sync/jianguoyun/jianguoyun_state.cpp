#include "jianguoyun_state.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "core/notebook.h"
#include "jianguoyun_transport.h"
#include "sync/sync_json_keys.h"
#include "sync/webdav/webdav_state.h"
#include "sync/webdav/webdav_transport.h"
#include "utils/file_utils.h"

namespace vxcore {
namespace jianguoyun {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using webdav::Check;
using webdav::CheckCancelled;
using webdav::HashBytes;
using webdav::HashFile;
using webdav::NewId;
using webdav::ReadBytes;
using webdav::Require;
namespace {

void Keys(const Json &j, std::initializer_list<const char *> keys) {
  Require(j.is_object() && j.size() == keys.size());
  for (const auto *key : keys) Require(j.contains(key));
}
std::string Text(const Json &j, const char *key) {
  Require(j.contains(key) && j.at(key).is_string());
  return j.at(key).get<std::string>();
}
uint64_t Number(const Json &j, const char *key) {
  Require(j.contains(key) && j.at(key).is_number_integer());
  if (j.at(key).is_number_unsigned()) return j.at(key).get<uint64_t>();
  const auto value = j.at(key).get<int64_t>();
  Require(value >= 0);
  return static_cast<uint64_t>(value);
}
void Uuid(const std::string &id) { Require(NotebookEncryption::IsCanonicalUuid(id)); }
std::string NullableText(const Json &j, const char *key) {
  Require(j.contains(key));
  return j.at(key).is_null() ? std::string() : Text(j, key);
}
Json Nullable(const std::string &value) { return value.empty() ? Json(nullptr) : Json(value); }
void SnapshotName(const std::string &name) {
  Check(WebDavTransport::ValidateRelativePath(name));
  const auto parts = SplitPathComponents(name);
  Require(parts.size() == 3 && parts[0] == "snapshots");
  Uuid(parts[1]);
  Uuid(parts[2]);
}
// AtomicFileWriter's only uncommitted sibling grammar. A crash may leave these
// beside a valid state/snapshot; they are never interpreted as committed journal data.
std::string AtomicTarget(const std::string &name) {
  const auto marker = name.rfind(".tmp-");
  if (marker == std::string::npos) return {};
  const auto suffix = name.substr(marker + 5);
  const auto dash = suffix.find('-');
  if (dash == std::string::npos || dash == 0 || dash + 1 == suffix.size() ||
      suffix.find('-', dash + 1) != std::string::npos)
    return {};
  for (size_t index = 0; index < suffix.size(); ++index)
    if (index != dash && (suffix[index] < '0' || suffix[index] > '9')) return {};
  return name.substr(0, marker);
}
void StageName(const std::string &name) {
  Check(WebDavTransport::ValidateRelativePath(name));
  const auto parts = SplitPathComponents(name);
  Require(parts.size() == 4 && parts[0] == ".vnote-sync" && parts[1] == "staging");
  Uuid(parts[2]);
  Uuid(parts[3]);
}
std::string NativeKey(const std::string &path) {
#ifdef _WIN32
  const auto wide = PathFromUtf8(path).native();
  const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data(),
                                  static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr, 0);
  Require(count > 0, VXCORE_ERR_UNSUPPORTED);
  std::wstring mapped(static_cast<size_t>(count), L'\0');
  Require(
      LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data(),
                    static_cast<int>(wide.size()), mapped.data(), count, nullptr, nullptr, 0) > 0,
      VXCORE_ERR_UNSUPPORTED);
  return PathToGenericUtf8(fs::path(mapped));
#elif defined(__APPLE__)
  auto input =
      CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(path.data()),
                              static_cast<CFIndex>(path.size()), kCFStringEncodingUTF8, false);
  Require(input != nullptr, VXCORE_ERR_UNSUPPORTED);
  auto normalized = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, input);
  CFRelease(input);
  Require(normalized != nullptr, VXCORE_ERR_UNSUPPORTED);
  CFStringNormalize(normalized, kCFStringNormalizationFormD);
  CFStringFold(normalized, kCFCompareCaseInsensitive, nullptr);
  const auto capacity =
      CFStringGetMaximumSizeForEncoding(CFStringGetLength(normalized), kCFStringEncodingUTF8) + 1;
  std::string result(static_cast<size_t>(capacity), '\0');
  const bool ok = CFStringGetCString(normalized, result.data(), capacity, kCFStringEncodingUTF8);
  CFRelease(normalized);
  Require(ok, VXCORE_ERR_UNSUPPORTED);
  result.resize(std::strlen(result.c_str()));
  return result;
#else
  return path;
#endif
}
Json EncodeTree(const Tree &tree) {
  auto j = Json::object();
  for (const auto &item : tree) j[item.first] = EncodeEntry(item.second);
  return j;
}
Tree DecodeTree(const Json &j, bool complete = true) {
  Require(j.is_object() && j.size() <= kMaxEntries);
  Tree result;
  for (auto it = j.begin(); it != j.end(); ++it) {
    ValidatePath(it.key());
    result.emplace(it.key(), DecodeEntry(it.value()));
  }
  if (complete)
    ValidateTree(result);
  else {
    // A baseline is a set of individually acknowledged facts, not a complete manifest.
    // Partial Apply may have installed a child while a common parent is not yet recorded.
    std::set<std::string> native;
    for (const auto &item : result) Require(native.insert(NativeKey(item.first)).second);
  }
  return result;
}
Json EncodeVersion(const Version &v) {
  Json entry = v.entry.kind == "absent" ? Json(nullptr) : EncodeEntry(v.entry);
  return {{"entry", entry},
          {"snapshot", Nullable(v.snapshot)},
          {"rawSha256", Nullable(v.raw_sha256)},
          {kJsonKeyModifiedUtc, v.modified_utc},
          {"binary", v.binary}};
}
Version DecodeVersion(const Json &j, const State &state, std::map<std::string, Entry> &verified,
                      bool verify_payloads) {
  Keys(j, {"entry", "snapshot", "rawSha256", kJsonKeyModifiedUtc, "binary"});
  Version v;
  if (!j.at("entry").is_null()) v.entry = DecodeEntry(j.at("entry"));
  v.snapshot = NullableText(j, "snapshot");
  v.raw_sha256 = NullableText(j, "rawSha256");
  Require(v.raw_sha256.empty() || IsHash(v.raw_sha256));
  Require(j.at(kJsonKeyModifiedUtc).is_number_integer() && j.at("binary").is_boolean());
  v.modified_utc = j.at(kJsonKeyModifiedUtc).get<int64_t>();
  v.binary = j.at("binary").get<bool>();
  if (!v.snapshot.empty()) {
    Require(v.entry.kind == "file");
    SnapshotName(v.snapshot);
    const auto known = verified.find(v.snapshot);
    if (known == verified.end()) {
      state.VerifySnapshot(v.snapshot, v.entry.sha256, v.entry.size, verify_payloads);
      if (verify_payloads) {
        const auto chunks = HashChunks(state.Resolve(v.snapshot), state.cancellation);
        Require(chunks.size() == v.entry.chunks.size());
        for (size_t index = 0; index < chunks.size(); ++index)
          Require(chunks[index].sha256 == v.entry.chunks[index].sha256 &&
                  chunks[index].size == v.entry.chunks[index].size);
      }
      verified.emplace(v.snapshot, v.entry);
    } else {
      Require(Same(known->second, v.entry) && known->second.chunks.size() == v.entry.chunks.size());
      for (size_t index = 0; index < v.entry.chunks.size(); ++index)
        Require(known->second.chunks[index].sha256 == v.entry.chunks[index].sha256 &&
                known->second.chunks[index].size == v.entry.chunks[index].size);
    }
  }
  Require(v.entry.kind == "file" || (v.snapshot.empty() && v.raw_sha256.empty()));
  return v;
}
Json EncodeVersions(const Versions &versions) {
  auto j = Json::object();
  for (const auto &item : versions) j[item.first] = EncodeVersion(item.second);
  return j;
}
Versions DecodeVersions(const Json &j, const State &state, std::map<std::string, Entry> &verified,
                        bool verify_payloads) {
  Require(j.is_object() && j.size() <= kMaxEntries);
  Versions result;
  for (auto it = j.begin(); it != j.end(); ++it) {
    ValidatePath(it.key());
    result.emplace(it.key(), DecodeVersion(it.value(), state, verified, verify_payloads));
  }
  ValidateTree(Entries(result));
  return result;
}
Json OptionalHead(const std::optional<Head> &head) {
  return head ? EncodeHead(*head) : Json(nullptr);
}
std::optional<Head> ReadHead(const Json &j) {
  return j.is_null() ? std::nullopt : std::optional<Head>(DecodeHead(j));
}
void CheckHeadBinding(const Head &h, const State &s) {
  Require(h.notebook_id == s.notebook_id &&
          (s.repository_id.empty() || h.repository_id == s.repository_id));
}
void ValidateBinding(const Json &j, const State &s, bool bootstrap_transition = false) {
  Require(Number(j, kJsonKeyVersion) == 1 && Text(j, kJsonKeyNotebookId) == s.notebook_id &&
          Text(j, kJsonKeyRemoteUrl) == s.remote_url && Text(j, "usernameHash") == s.username_hash);
  const auto repository = Text(j, "repositoryId");
  Require(repository == s.repository_id || (bootstrap_transition && repository.empty()));
}
void CheckProbe(const Json &j, const State &state, bool verify_payloads = true) {
  Keys(j, {kJsonKeyVersion, kJsonKeyNotebookId, kJsonKeyRemoteUrl, "usernameHash", "repositoryId",
           "operationId", kJsonKeyResources});
  ValidateBinding(j, state);
  Uuid(Text(j, "operationId"));
  const auto &resources = j.at(kJsonKeyResources);
  Require(resources.is_object() && resources.size() >= 3 && resources.size() <= 8);
  for (auto it = resources.begin(); it != resources.end(); ++it) {
    StageName(it.key());
    Require(SplitPathComponents(it.key())[2] == Text(j, "operationId"));
    Keys(it.value(), {"snapshot", "sha256", "size"});
    state.VerifySnapshot(Text(it.value(), "snapshot"), Text(it.value(), "sha256"),
                         Number(it.value(), "size"), verify_payloads);
  }
}
}  // namespace

Json ParseRecord(const std::string &bytes, size_t limit) {
  Require(bytes.size() <= limit, VXCORE_ERR_UNSUPPORTED);
  std::vector<std::set<std::string>> objects;
  return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json &parsed) {
    Require(depth <= 64, VXCORE_ERR_UNSUPPORTED);
    if (event == Json::parse_event_t::object_start)
      objects.emplace_back();
    else if (event == Json::parse_event_t::key) {
      Require(!objects.empty() && objects.back().insert(parsed.get<std::string>()).second);
    } else if (event == Json::parse_event_t::object_end) {
      Require(!objects.empty());
      objects.pop_back();
    }
    return true;
  });
}

bool IsHash(const std::string &value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
void ValidatePath(const std::string &path) {
  Check(WebDavTransport::ValidateRelativePath(path));
  static const SyncConfig defaults;
  Require(SplitPathComponents(path).size() <= kMaxDepth && !webdav::IsExcluded(path, defaults) &&
              !webdav::IsScratch(path) && path != ".vnote-sync" &&
              path.compare(0, 12, ".vnote-sync/") != 0,
          VXCORE_ERR_UNSUPPORTED);
}
bool Present(const Entry &entry) { return entry.kind == "file" || entry.kind == "directory"; }
bool Same(const Entry &a, const Entry &b) {
  if (!Present(a) && !Present(b)) return true;
  return a.kind == b.kind && (a.kind != "file" || (a.size == b.size && a.sha256 == b.sha256));
}
bool SameRevision(const Entry &a, const Entry &b) { return Same(a, b) && a.revision == b.revision; }
Entry Lookup(const Tree &tree, const std::string &path) {
  const auto it = tree.find(path);
  return it == tree.end() ? Entry{} : it->second;
}
Version Lookup(const Versions &tree, const std::string &path) {
  const auto it = tree.find(path);
  return it == tree.end() ? Version{} : it->second;
}
Tree Entries(const Versions &versions) {
  Tree result;
  for (const auto &item : versions)
    if (item.second.entry.kind != "absent") result.emplace(item.first, item.second.entry);
  return result;
}
void ValidateTree(const Tree &tree) {
  Require(tree.size() <= kMaxEntries, VXCORE_ERR_UNSUPPORTED);
  std::set<std::string> native;
  for (const auto &item : tree) {
    ValidatePath(item.first);
    Require(native.insert(NativeKey(item.first)).second, VXCORE_ERR_UNSUPPORTED);
    if (!Present(item.second)) continue;
    auto parent = PathFromUtf8(item.first).parent_path();
    while (!parent.empty()) {
      const auto it = tree.find(PathToGenericUtf8(parent));
      Require(it != tree.end() && it->second.kind == "directory", VXCORE_ERR_UNSUPPORTED);
      parent = parent.parent_path();
    }
  }
}
Json EncodeHead(const Head &h) {
  return {{key::kFormat, kFormat},
          {key::kFormatVersion, kFormatVersion},
          {key::kRepositoryId, h.repository_id},
          {kJsonKeyNotebookId, h.notebook_id},
          {key::kGeneration, h.generation},
          {key::kCommitHash, h.commit_hash}};
}
Head DecodeHead(const Json &j) {
  Keys(j, {key::kFormat, key::kFormatVersion, key::kRepositoryId, kJsonKeyNotebookId,
           key::kGeneration, key::kCommitHash});
  Require(Text(j, key::kFormat) == kFormat && Number(j, key::kFormatVersion) == kFormatVersion);
  Head h{Text(j, key::kRepositoryId), Text(j, kJsonKeyNotebookId), Number(j, key::kGeneration),
         Text(j, key::kCommitHash)};
  Uuid(h.repository_id);
  Uuid(h.notebook_id);
  Require(h.generation > 0 && IsHash(h.commit_hash));
  return h;
}
Json EncodeEntry(const Entry &entry) {
  auto chunks = Json::array();
  for (const auto &chunk : entry.chunks)
    chunks.push_back({{key::kSha256, chunk.sha256}, {key::kSize, chunk.size}});
  return {{kJsonKeyKind, entry.kind},
          {key::kRevision, entry.revision},
          {key::kSize, entry.size},
          {key::kSha256, Nullable(entry.sha256)},
          {key::kChunks, std::move(chunks)}};
}
Entry DecodeEntry(const Json &j, bool allow_absent) {
  if (allow_absent && j.is_null()) return {};
  Keys(j, {kJsonKeyKind, key::kRevision, key::kSize, key::kSha256, key::kChunks});
  Entry e;
  e.kind = Text(j, kJsonKeyKind);
  e.revision = Text(j, key::kRevision);
  Uuid(e.revision);
  Require(e.kind == "file" || e.kind == "directory" || e.kind == "tombstone");
  e.size = Number(j, key::kSize);
  Require(e.size <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
  e.sha256 = NullableText(j, key::kSha256);
  const auto &chunks = j.at(key::kChunks);
  Require(chunks.is_array());
  if (e.kind != "file") {
    Require(e.size == 0 && e.sha256.empty() && chunks.empty());
    return e;
  }
  Require(IsHash(e.sha256) && chunks.size() == e.size / kChunkBytes + (e.size % kChunkBytes != 0));
  uint64_t remaining = e.size;
  for (const auto &chunk : chunks) {
    Keys(chunk, {key::kSha256, key::kSize});
    Chunk c{Text(chunk, key::kSha256), Number(chunk, key::kSize)};
    Require(IsHash(c.sha256) && c.size == std::min(remaining, kChunkBytes) && c.size > 0);
    remaining -= c.size;
    e.chunks.push_back(std::move(c));
  }
  Require(remaining == 0 && (e.size != 0 || e.sha256 == HashBytes("")));
  return e;
}
std::string EncodeCommit(const Commit &commit) {
  ValidateTree(commit.entries);
  auto j = EncodeHead(commit.identity);
  j.erase(key::kCommitHash);
  j[key::kOperationId] = commit.operation_id;
  j[kJsonKeyParent] = Nullable(commit.parent);
  j[key::kEntries] = EncodeTree(commit.entries);
  auto bytes = j.dump();
  Require(bytes.size() <= kManifestBytes, VXCORE_ERR_UNSUPPORTED);
  return bytes;
}
Commit DecodeCommit(const std::string &bytes, const std::string &expected_hash) {
  Require(bytes.size() <= kManifestBytes && IsHash(expected_hash) &&
          HashBytes(bytes) == expected_hash);
  const auto j = ParseRecord(bytes);
  Keys(j, {key::kFormat, key::kFormatVersion, key::kRepositoryId, kJsonKeyNotebookId,
           key::kGeneration, key::kOperationId, kJsonKeyParent, key::kEntries});
  auto hj = j;
  hj.erase(key::kOperationId);
  hj.erase(kJsonKeyParent);
  hj.erase(key::kEntries);
  hj[key::kCommitHash] = expected_hash;
  Commit result;
  result.identity = DecodeHead(hj);
  result.operation_id = Text(j, key::kOperationId);
  Uuid(result.operation_id);
  result.parent = NullableText(j, kJsonKeyParent);
  Require(result.identity.generation == 1 ? result.parent.empty() : IsHash(result.parent));
  result.entries = DecodeTree(j.at(key::kEntries));
  Require(EncodeCommit(result) == bytes);
  return result;
}
std::string ObjectPath(const std::string &hash) {
  Require(IsHash(hash));
  return ".vnote-sync/objects/" + hash.substr(0, 2) + "/" + hash;
}
std::string CommitPath(const std::string &hash) {
  Require(IsHash(hash));
  return ".vnote-sync/commits/" + hash + ".json";
}
std::string StagingPath(const std::string &operation_id) {
  Uuid(operation_id);
  return ".vnote-sync/staging/" + operation_id + "/" + NewId();
}
std::vector<Chunk> HashChunks(const fs::path &file, const SyncCancellationPtr &token) {
  Require(sodium_init() >= 0);
  std::ifstream input(file, std::ios::binary);
  Require(input.good(), VXCORE_ERR_IO);
  uint64_t remaining = fs::file_size(file);
  std::vector<Chunk> chunks;
  std::array<unsigned char, 64 * 1024> buffer{};
  while (remaining) {
    const auto length = std::min(remaining, kChunkBytes);
    crypto_hash_sha256_state context;
    Require(crypto_hash_sha256_init(&context) == 0);
    uint64_t todo = length;
    while (todo) {
      CheckCancelled(token);
      const auto count = static_cast<size_t>(std::min<uint64_t>(todo, buffer.size()));
      input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(count));
      Require(input.gcount() == static_cast<std::streamsize>(count) && !input.bad(), VXCORE_ERR_IO);
      Require(crypto_hash_sha256_update(&context, buffer.data(), count) == 0);
      todo -= count;
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
    Require(crypto_hash_sha256_final(&context, hash.data()) == 0);
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
    chunks.push_back({hex.data(), length});
    remaining -= length;
  }
  Require(input.peek() == std::char_traits<char>::eof(), VXCORE_ERR_SYNC_IN_PROGRESS);
  return chunks;
}

fs::path State::Resolve(const std::string &relative) const {
  fs::path path;
  Check(WebDavTransport::ResolveLocalPath(directory, relative, path));
  return path;
}
Json State::Binding() const {
  return {{kJsonKeyVersion, 1},
          {kJsonKeyNotebookId, notebook_id},
          {kJsonKeyRemoteUrl, remote_url},
          {"usernameHash", username_hash},
          {"repositoryId", repository_id}};
}
void State::Open(const fs::path &notebook_root, const std::string &id, const std::string &url,
                 const std::string &username) {
  root = notebook_root;
  fs::path path;
  Check(WebDavTransport::ResolveLocalPath(root, kPrivatePath, path));
  if (fs::exists(path / "state.json")) {
    LoadDirectory(path);
    Require(notebook_id == id && remote_url == url);
    if (username_hash != username)
      Require(!pending && conflicts.empty() && !fs::exists(Resolve("probe.json")),
              VXCORE_ERR_SYNC_IN_PROGRESS);
  } else {
    if (fs::exists(path)) {
      directory = path;
      ValidateFiles();
      for (const auto &entry : fs::directory_iterator(path))
        Require(entry.path().filename() == "retired");
    }
    notebook_id = id;
    remote_url = url;
    username_hash = username;
    repository_id.clear();
    head.reset();
    entries.clear();
    conflicts.clear();
    pending.reset();
    webdav::EnsureDirectory(root, kPrivatePath);
    directory = path;
    Save();
  }
}
void State::LoadDirectory(const fs::path &path, bool verify_payloads) {
  directory = path;
  ValidateFiles();
  const auto j = ParseRecord(ReadBytes(Resolve("state.json"), kManifestBytes));
  Keys(j, {kJsonKeyVersion, kJsonKeyNotebookId, kJsonKeyRemoteUrl, "usernameHash", "repositoryId",
           "head", "entries", "conflicts"});
  Require(Number(j, kJsonKeyVersion) == 1);
  notebook_id = Text(j, kJsonKeyNotebookId);
  Uuid(notebook_id);
  remote_url = Text(j, kJsonKeyRemoteUrl);
  std::string canonical;
  Check(JianguoyunTransport::CanonicalizeUrl(remote_url, canonical));
  Require(canonical == remote_url);
  username_hash = Text(j, "usernameHash");
  Require(IsHash(username_hash));
  repository_id = Text(j, "repositoryId");
  if (!repository_id.empty()) Uuid(repository_id);
  head = ReadHead(j.at("head"));
  Require(repository_id.empty() == !head.has_value());
  if (head) CheckHeadBinding(*head, *this);
  entries = DecodeTree(j.at("entries"), false);
  Require(head || entries.empty());
  conflicts.clear();
  std::map<std::string, Entry> verified_snapshots;
  Require(j.at("conflicts").is_object() && j.at("conflicts").size() <= kMaxEntries);
  for (auto it = j.at("conflicts").begin(); it != j.at("conflicts").end(); ++it) {
    ValidatePath(it.key());
    const auto &c = it.value();
    Keys(c, {"local", "remote", kJsonKeyCanKeepBoth, "resolution"});
    Conflict conflict;
    conflict.local = DecodeVersion(c.at("local"), *this, verified_snapshots, verify_payloads);
    conflict.remote = DecodeVersion(c.at("remote"), *this, verified_snapshots, verify_payloads);
    Require(c.at(kJsonKeyCanKeepBoth).is_boolean());
    conflict.can_keep_both = c.at(kJsonKeyCanKeepBoth).get<bool>();
    Require(!conflict.can_keep_both ||
            (!webdav::IsMetadata(it.key()) && !webdav::IsEncrypted(it.key()) &&
             conflict.local.entry.kind == "file" && conflict.remote.entry.kind == "file"));
    for (const auto *v : {&conflict.local, &conflict.remote})
      Require(v->entry.kind != "file" || !v->snapshot.empty());
    conflict.resolution = Text(c, "resolution");
    Require(conflict.resolution.empty() || conflict.resolution == "keep_local" ||
            conflict.resolution == "keep_remote" ||
            (conflict.resolution == "keep_both" && conflict.can_keep_both));
    conflicts.emplace(it.key(), std::move(conflict));
  }
  pending.reset();
  if (fs::exists(Resolve("pending.json"))) {
    const auto p = ParseRecord(ReadBytes(Resolve("pending.json"), kManifestBytes));
    Keys(p, {kJsonKeyVersion, kJsonKeyNotebookId, kJsonKeyRemoteUrl, "usernameHash", "repositoryId",
             "operationId", "baseHead", "baseToken", "intendedHead", "headSnapshot", "headStaging",
             "phase", "freshBootstrap", "local", "target", "uploads", "apply"});
    Require(p.at("freshBootstrap").is_boolean());
    Pending next;
    next.fresh_bootstrap = p.at("freshBootstrap").get<bool>();
    ValidateBinding(p, *this, next.fresh_bootstrap);
    next.operation_id = Text(p, "operationId");
    Uuid(next.operation_id);
    next.base_head = ReadHead(p.at("baseHead"));
    next.base_token = Text(p, "baseToken");
    Require(next.base_head ? JianguoyunTransport::IsRawEtag(next.base_token)
                           : next.base_token.empty());
    if (next.base_head) CheckHeadBinding(*next.base_head, *this);
    next.intended_head = ReadHead(p.at("intendedHead"));
    if (next.intended_head) {
      CheckHeadBinding(*next.intended_head, *this);
      Require(next.base_head
                  ? next.intended_head->generation == next.base_head->generation + 1 &&
                        next.intended_head->repository_id == next.base_head->repository_id
                  : next.intended_head->generation == 1 && next.fresh_bootstrap);
    }
    next.head_snapshot = Text(p, "headSnapshot");
    next.head_staging = Text(p, "headStaging");
    Require(next.head_snapshot.empty() == next.head_staging.empty());
    if (!next.head_snapshot.empty()) {
      Require(next.intended_head.has_value());
      StageName(next.head_staging);
      Require(SplitPathComponents(next.head_staging)[2] == next.operation_id);
      SnapshotName(next.head_snapshot);
      const auto expected = EncodeHead(*next.intended_head).dump();
      VerifySnapshot(next.head_snapshot, HashBytes(expected), expected.size(), verify_payloads);
    }
    next.phase = Text(p, "phase");
    Require(next.phase == "prepared" || next.phase == "publishing" || next.phase == "published" ||
            next.phase == "applying");
    next.local = DecodeVersions(p.at("local"), *this, verified_snapshots, verify_payloads);
    next.target = DecodeVersions(p.at("target"), *this, verified_snapshots, verify_payloads);
    Require(p.at("uploads").is_object() && p.at("uploads").size() <= 1000000);
    std::map<std::string, std::vector<Chunk>> upload_chunks;
    for (auto it = p.at("uploads").begin(); it != p.at("uploads").end(); ++it) {
      const auto &u = it.value();
      Keys(u, {"canonical", "staging", "snapshot", "sha256", "offset", "size", "stage"});
      Upload upload{Text(u, "canonical"), Text(u, "staging"),  Text(u, "snapshot"),
                    Text(u, "sha256"),    Number(u, "offset"), Number(u, "size"),
                    Text(u, "stage")};
      Require(IsHash(upload.sha256) && it.key() == upload.canonical &&
              (upload.canonical == ObjectPath(upload.sha256) ||
               upload.canonical == CommitPath(upload.sha256)));
      StageName(upload.staging);
      Require(SplitPathComponents(upload.staging)[2] == next.operation_id);
      SnapshotName(upload.snapshot);
      Require(upload.stage == "prepared" || upload.stage == "putStarted" ||
              upload.stage == "stagedUploaded" || upload.stage == "moveStarted" ||
              upload.stage == "publishedVerified");
      const auto size = fs::file_size(Resolve(upload.snapshot));
      Require(upload.offset <= size && upload.size <= size - upload.offset &&
              (upload.canonical == ObjectPath(upload.sha256)
                   ? upload.size > 0 && upload.size <= kChunkBytes
                   : upload.offset == 0 && upload.size == size && size <= kManifestBytes));
      if (upload.canonical == ObjectPath(upload.sha256)) Require(upload.offset % kChunkBytes == 0);
      if (verify_payloads && upload.canonical == ObjectPath(upload.sha256)) {
        const auto verified = verified_snapshots.find(upload.snapshot);
        const std::vector<Chunk> *chunks = nullptr;
        if (verified != verified_snapshots.end())
          chunks = &verified->second.chunks;
        else {
          auto cached = upload_chunks.find(upload.snapshot);
          if (cached == upload_chunks.end())
            cached =
                upload_chunks
                    .emplace(upload.snapshot, HashChunks(Resolve(upload.snapshot), cancellation))
                    .first;
          chunks = &cached->second;
        }
        const auto index = static_cast<size_t>(upload.offset / kChunkBytes);
        Require(index < chunks->size() && chunks->at(index).sha256 == upload.sha256 &&
                chunks->at(index).size == upload.size);
      } else if (verify_payloads) {
        const auto commit =
            DecodeCommit(ReadBytes(Resolve(upload.snapshot), kManifestBytes), upload.sha256);
        Require(commit.identity.notebook_id == notebook_id &&
                commit.operation_id == next.operation_id);
        Require(repository_id.empty() || commit.identity.repository_id == repository_id ||
                next.fresh_bootstrap);
      }
      next.uploads.emplace(it.key(), std::move(upload));
    }
    if (next.intended_head) {
      const auto intended = next.uploads.find(CommitPath(next.intended_head->commit_hash));
      Require(intended != next.uploads.end());
      if (verify_payloads) {
        const auto commit =
            DecodeCommit(ReadBytes(Resolve(intended->second.snapshot), kManifestBytes),
                         next.intended_head->commit_hash);
        Require(EncodeHead(commit.identity) == EncodeHead(*next.intended_head) &&
                EncodeTree(commit.entries) == EncodeTree(Entries(next.target)) &&
                commit.parent == (next.base_head ? next.base_head->commit_hash : ""));
      }
      if (next.phase != "prepared") Require(intended->second.stage == "publishedVerified");
    } else
      Require(next.phase != "publishing");
    Require(p.at("apply").is_object() && p.at("apply").size() <= kMaxEntries);
    for (auto it = p.at("apply").begin(); it != p.at("apply").end(); ++it) {
      ValidatePath(it.key());
      const auto &a = it.value();
      Keys(a, {"expected", "desired", "backup", "backupSha256", "stage"});
      ApplyOperation op;
      op.expected = DecodeVersion(a.at("expected"), *this, verified_snapshots, verify_payloads);
      op.desired = DecodeVersion(a.at("desired"), *this, verified_snapshots, verify_payloads);
      op.backup = Text(a, "backup");
      op.backup_sha256 = Text(a, "backupSha256");
      if (!op.backup.empty()) {
        Require(op.expected.entry.kind == "file" && op.backup_sha256 == op.expected.raw_sha256);
        VerifySnapshot(op.backup, op.backup_sha256, std::nullopt, verify_payloads);
      } else
        Require(op.backup_sha256.empty());
      op.stage = Text(a, "stage");
      Require(op.stage == "prepared" || op.stage == "installing" || op.stage == "localConfirmed" ||
              op.stage == "baselineConfirmed");
      Require(op.desired.entry.kind != "file" || !op.desired.snapshot.empty());
      Require(SameRevision(op.desired.entry, Lookup(next.target, it.key()).entry));
      next.apply.emplace(it.key(), std::move(op));
    }
    if (next.phase == "published" || next.phase == "applying")
      Require(next.intended_head || next.base_head);
    pending = std::move(next);
  }
  if (fs::exists(Resolve("probe.json")))
    CheckProbe(ParseRecord(ReadBytes(Resolve("probe.json"), 1024 * 1024)), *this, verify_payloads);
}
void State::Save() const {
  auto j = Binding();
  j["head"] = OptionalHead(head);
  j["entries"] = EncodeTree(entries);
  j["conflicts"] = Json::object();
  for (const auto &item : conflicts) {
    const auto &c = item.second;
    j["conflicts"][item.first] = {{"local", EncodeVersion(c.local)},
                                  {"remote", EncodeVersion(c.remote)},
                                  {kJsonKeyCanKeepBoth, c.can_keep_both},
                                  {"resolution", c.resolution}};
  }
  const auto bytes = j.dump();
  Require(bytes.size() <= kManifestBytes, VXCORE_ERR_UNSUPPORTED);
  Check(WriteFileAtomic(Resolve("state.json"), bytes));
}
void State::SavePending() const {
  Require(pending.has_value());
  const auto &p = *pending;
  auto j = Binding();
  j["operationId"] = p.operation_id;
  j["baseHead"] = OptionalHead(p.base_head);
  j["baseToken"] = p.base_token;
  j["intendedHead"] = OptionalHead(p.intended_head);
  j["headSnapshot"] = p.head_snapshot;
  j["headStaging"] = p.head_staging;
  j["phase"] = p.phase;
  j["freshBootstrap"] = p.fresh_bootstrap;
  j["local"] = EncodeVersions(p.local);
  j["target"] = EncodeVersions(p.target);
  j["uploads"] = Json::object();
  for (const auto &item : p.uploads) {
    const auto &u = item.second;
    j["uploads"][item.first] = {{"canonical", u.canonical}, {"staging", u.staging},
                                {"snapshot", u.snapshot},   {"sha256", u.sha256},
                                {"offset", u.offset},       {"size", u.size},
                                {"stage", u.stage}};
  }
  j["apply"] = Json::object();
  for (const auto &item : p.apply) {
    const auto &a = item.second;
    j["apply"][item.first] = {{"expected", EncodeVersion(a.expected)},
                              {"desired", EncodeVersion(a.desired)},
                              {"backup", a.backup},
                              {"backupSha256", a.backup_sha256},
                              {"stage", a.stage}};
  }
  const auto bytes = j.dump();
  Require(bytes.size() <= kManifestBytes, VXCORE_ERR_UNSUPPORTED);
  Check(WriteFileAtomic(Resolve("pending.json"), bytes));
}
void State::ClearPending() {
  std::error_code error;
  fs::remove(Resolve("pending.json"), error);
  Require(!error, VXCORE_ERR_IO);
  pending.reset();
}
void State::RotateUsername(const std::string &hash) {
  Require(IsHash(hash));
  if (hash == username_hash) return;
  Require(!pending && conflicts.empty() && !fs::exists(Resolve("probe.json")),
          VXCORE_ERR_SYNC_IN_PROGRESS);
  const auto previous = username_hash;
  username_hash = hash;
  try {
    Save();
  } catch (...) {
    username_hash = previous;
    throw;
  }
}
std::string State::AllocateSnapshot() {
  const auto folder = "snapshots/" + (pending ? pending->operation_id : NewId());
  webdav::EnsureDirectory(directory, folder);
  const auto name = folder + "/" + NewId();
  Require(!fs::exists(Resolve(name)));
  Check(WriteFileAtomic(Resolve(name), ""));
  return name;
}
std::string State::SnapshotBytes(const std::string &bytes) {
  const auto name = AllocateSnapshot();
  Check(WriteFileAtomic(Resolve(name), bytes));
  return name;
}
std::string State::SnapshotFile(const fs::path &file) {
  const auto name = AllocateSnapshot();
  webdav::CopyAtomic(file, Resolve(name), cancellation);
  return name;
}
void State::VerifySnapshot(const std::string &name, const std::string &hash,
                           std::optional<uint64_t> size, bool verify_hash) const {
  SnapshotName(name);
  Require(IsHash(hash));
  const auto file = Resolve(name);
  Require(fs::is_regular_file(file) && (!size || fs::file_size(file) == *size) &&
          (!verify_hash || HashFile(file, cancellation) == hash));
}
void State::ValidateFiles() const {
  auto ancestor = fs::absolute(directory).root_path();
  Require(CheckReparsePoint(PathToUtf8(ancestor)) == ReparseState::kNo, VXCORE_ERR_UNSUPPORTED);
  for (const auto &component : fs::absolute(directory).relative_path()) {
    ancestor /= component;
    Require(CheckReparsePoint(PathToUtf8(ancestor)) == ReparseState::kNo, VXCORE_ERR_UNSUPPORTED);
  }
  Require(
      fs::is_directory(directory) && CheckReparsePoint(PathToUtf8(directory)) == ReparseState::kNo,
      VXCORE_ERR_UNSUPPORTED);
  for (const auto &item : fs::directory_iterator(directory)) {
    const auto name = PathToUtf8(item.path().filename());
    Require(CheckReparsePoint(PathToUtf8(item.path())) == ReparseState::kNo,
            VXCORE_ERR_UNSUPPORTED);
    if (name == "retired") {
      Require(item.is_directory());
      for (const auto &archive : fs::directory_iterator(item.path())) {
        Uuid(PathToUtf8(archive.path().filename()));
        Require(CheckReparsePoint(PathToUtf8(archive.path())) == ReparseState::kNo &&
                    archive.is_directory(),
                VXCORE_ERR_UNSUPPORTED);
      }
    } else if (name == "snapshots") {
      Require(item.is_directory());
      for (const auto &folder : fs::directory_iterator(item.path())) {
        Uuid(PathToUtf8(folder.path().filename()));
        Require(CheckReparsePoint(PathToUtf8(folder.path())) == ReparseState::kNo &&
                    folder.is_directory(),
                VXCORE_ERR_UNSUPPORTED);
        for (const auto &file : fs::directory_iterator(folder.path())) {
          const auto filename = PathToUtf8(file.path().filename());
          const auto target = AtomicTarget(filename);
          Uuid(target.empty() ? filename : target);
          Require(CheckReparsePoint(PathToUtf8(file.path())) == ReparseState::kNo &&
                      file.is_regular_file(),
                  VXCORE_ERR_UNSUPPORTED);
        }
      }
    } else {
      const auto target = AtomicTarget(name);
      const auto &recognized = target.empty() ? name : target;
      Require((recognized == "state.json" || recognized == "pending.json" ||
               recognized == "probe.json") &&
                  item.is_regular_file(),
              VXCORE_ERR_UNSUPPORTED);
    }
  }
}
void State::CollectSnapshots() const {
  std::set<std::string> keep;
  auto version = [&](const Version &v) {
    if (!v.snapshot.empty()) keep.insert(v.snapshot);
  };
  for (const auto &item : conflicts) {
    version(item.second.local);
    version(item.second.remote);
  }
  if (pending) {
    for (const auto &item : pending->local) version(item.second);
    for (const auto &item : pending->target) version(item.second);
    for (const auto &item : pending->uploads) keep.insert(item.second.snapshot);
    for (const auto &item : pending->apply) {
      version(item.second.expected);
      version(item.second.desired);
      if (!item.second.backup.empty()) keep.insert(item.second.backup);
    }
    if (!pending->head_snapshot.empty()) keep.insert(pending->head_snapshot);
  }
  if (fs::exists(Resolve("probe.json"))) {
    const auto j = ParseRecord(ReadBytes(Resolve("probe.json"), 1024 * 1024));
    CheckProbe(j, *this);
    for (const auto &resource : j.at(kJsonKeyResources)) keep.insert(Text(resource, "snapshot"));
  }
  ValidateFiles();
  for (const auto &file : fs::directory_iterator(directory)) {
    const auto name = PathToUtf8(file.path().filename());
    if (!AtomicTarget(name).empty()) Require(fs::remove(file.path()), VXCORE_ERR_IO);
  }
  const auto snapshots = Resolve("snapshots");
  if (!fs::exists(snapshots)) return;
  for (const auto &folder : fs::directory_iterator(snapshots)) {
    for (const auto &file : fs::directory_iterator(folder.path())) {
      const auto name = PathToGenericUtf8(file.path().lexically_relative(directory));
      if (!keep.count(name)) Require(fs::remove(file.path()), VXCORE_ERR_IO);
    }
    if (fs::is_empty(folder.path())) Require(fs::remove(folder.path()), VXCORE_ERR_IO);
  }
}
VxCoreError InspectRecovery(const fs::path &directory, RecoveryStatus &out) {
  out = {};
  try {
    std::error_code error;
    const auto type = fs::symlink_status(directory, error);
    if (error == std::errc::no_such_file_or_directory || type.type() == fs::file_type::not_found)
      return VXCORE_OK;
    Require(!error, VXCORE_ERR_IO);
    State state;
    if (!fs::exists(directory / "state.json")) {
      state.directory = directory;
      state.ValidateFiles();
      // Completed retirement leaves an archive container, not an active binding.
      for (const auto &entry : fs::directory_iterator(directory))
        Require(entry.path().filename() == "retired");
      return VXCORE_OK;
    }
    // Guards run on frequent protected-read/GUI paths: validate the complete typed
    // journal, bindings, names, file kinds and lengths here, not attachment contents.
    // Backend Open, transfer and Apply perform the streaming SHA-256 verification.
    state.LoadDirectory(directory, false);
    RecoveryStatus status;
    status.any_conflict = !state.conflicts.empty();
    status.any_pending =
        state.pending.has_value() || status.any_conflict || fs::exists(state.Resolve("probe.json"));
    status.encryption_conflict = state.conflicts.count(kEncryptionPath) != 0;
    status.encryption_pending = status.encryption_conflict;
    if (state.pending) {
      const auto &p = *state.pending;
      const auto local = Lookup(p.local, kEncryptionPath).entry;
      const auto target = Lookup(p.target, kEncryptionPath).entry;
      const auto base = Lookup(state.entries, kEncryptionPath);
      const bool target_known = !p.target.empty() || p.base_head || p.intended_head;
      if (!Same(local, base) || (target_known && !Same(target, base)) ||
          p.apply.count(kEncryptionPath))
        status.encryption_pending = true;
      // Outgoing key publication (including Keep Local) does not replace this device's
      // envelope and must not revoke its active leases. Only an incoming change does.
      status.replaces_encryption = target_known && !Same(local, target);
    }
    out = status;
    return VXCORE_OK;
  } catch (const webdav::Failure &failure) {
    return failure.error;
  } catch (const fs::filesystem_error &) {
    return VXCORE_ERR_IO;
  } catch (...) {
    return VXCORE_ERR_INVALID_STATE;
  }
}

}  // namespace jianguoyun
}  // namespace vxcore
