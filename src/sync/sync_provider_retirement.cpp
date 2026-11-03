#include "sync_provider_retirement.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "core/encryptednote.h"
#include "core/notebook.h"
#include "sync/http/sync_http_client.h"
#include "sync/sync_json_keys.h"
#include "sync/webdav/webdav_state.h"
#include "sync/webdav/webdav_transport.h"
#include "utils/file_utils.h"
#include "vxcore/notebook_json_keys.h"

namespace vxcore {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using webdav::Check;
using webdav::Require;
constexpr uint64_t kMaxNodes = 500000;
constexpr uint64_t kMaxBytes = uint64_t{8} * 1024 * 1024 * 1024;
constexpr uint64_t kMaxJson = 128 * 1024 * 1024;
constexpr uint64_t kMaxSnapshot = uint64_t{2} * 1024 * 1024 * 1024;
constexpr const char *kKeyPath = "vx_notebook/encryption.vne";

bool Uuid(const std::string &value) { return EncryptedNote::IsCanonicalUuid(value); }
bool Hash(const std::string &value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
void Keys(const Json &value, std::initializer_list<const char *> required,
          std::initializer_list<const char *> optional = {}) {
  Require(value.is_object());
  for (const auto *key : required) Require(value.contains(key));
  for (auto it = value.begin(); it != value.end(); ++it) {
    const auto contains = [&](auto keys) {
      return std::any_of(keys.begin(), keys.end(), [&](const char *key) { return it.key() == key; });
    };
    Require(contains(required) || contains(optional));
  }
}
std::string Text(const Json &value, const char *key, bool nullable = false) {
  const auto &field = value.at(key);
  if (nullable && field.is_null()) return {};
  Require(field.is_string());
  return field.get<std::string>();
}
std::string HashField(const Json &value, const char *key, bool nullable = false) {
  const auto text = Text(value, key, nullable);
  Require((nullable && value.at(key).is_null()) || Hash(text));
  return text;
}
bool KeyPath(const std::string &path) {
  const std::string_view key(kKeyPath);
  return path.size() == key.size() &&
         std::equal(path.begin(), path.end(), key.begin(), [](char a, char b) {
           return (a >= 'A' && a <= 'Z' ? a + 'a' - 'A' : a) == b;
         });
}
void LogicalPath(const std::string &path, bool scratch = false) {
  Check(WebDavTransport::ValidateRelativePath(path));
  static const SyncConfig defaults;
  Require(!webdav::IsExcluded(path, defaults) && (scratch || !webdav::IsScratch(path)));
}
bool Scratch(const std::string &path) {
  const std::string_view prefix(webdav::kScratchPrefix);
  return path.compare(0, prefix.size(), prefix) == 0 && Uuid(path.substr(prefix.size()));
}
bool Present(const fs::path &path) {
  return private_io::Exists(path);
}
FileIdentity Identity(const fs::path &path) {
  FileIdentity identity;
  Check(GetFileIdentity(path, identity, FileAccess::kPrivate));
  Require(identity.directory || identity.links == 1, VXCORE_ERR_UNSUPPORTED);
  return identity;
}
void Ancestors(const fs::path &path) {
  private_io::Validate(path);
  Require(Identity(path).directory);
}

struct Node {
  FileIdentity identity;
  uint64_t size = 0;
  std::string hash;
  std::string bytes;
};
// Open the leaf without following links and compare the opened object to both
// pathname identities. Parent components are separately checked before every read.
Node ReadNode(const fs::path &path, bool json) {
  Node result;
  result.identity = Identity(path);
  if (result.identity.directory) return result;
  Ancestors(path.parent_path());
  Require(sodium_init() >= 0);
  crypto_hash_sha256_state hash;
  Require(crypto_hash_sha256_init(&hash) == 0);
  const uint64_t limit = json ? kMaxJson : kMaxSnapshot;
  std::array<unsigned char, 65536> buffer{};
#ifdef _WIN32
  struct Handle {
    HANDLE value;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  };
  const NativePathArgument native(path, FileAccess::kPrivate);
  Handle file{CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                     FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
  Require(file.value != INVALID_HANDLE_VALUE, VXCORE_ERR_IO);
  BY_HANDLE_FILE_INFORMATION info{};
  FILE_ID_INFO file_id{};
  Require(GetFileInformationByHandle(file.value, &info) &&
          GetFileInformationByHandleEx(file.value, FileIdInfo, &file_id, sizeof(file_id)) &&
          !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
          info.nNumberOfLinks == 1, VXCORE_ERR_INVALID_STATE);
  uint64_t low = 0, high = 0;
  std::memcpy(&low, file_id.FileId.Identifier, sizeof(low));
  std::memcpy(&high, file_id.FileId.Identifier + sizeof(low), sizeof(high));
  Require(result.identity.device == file_id.VolumeSerialNumber &&
          result.identity.low == low && result.identity.high == high);
#else
  struct Handle {
    int value;
    ~Handle() { if (value >= 0) close(value); }
  } file{open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
  Require(file.value >= 0, VXCORE_ERR_IO);
  struct stat info{};
  Require(fstat(file.value, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1 &&
          result.identity.device == static_cast<uint64_t>(info.st_dev) &&
          result.identity.low == static_cast<uint64_t>(info.st_ino));
#endif
  for (;;) {
#ifdef _WIN32
    DWORD read = 0;
    Require(::ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr),
            VXCORE_ERR_IO);
    const auto count = static_cast<size_t>(read);
#else
    const auto read = ::read(file.value, buffer.data(), buffer.size());
    if (read < 0 && errno == EINTR) continue;
    Require(read >= 0, VXCORE_ERR_IO);
    const auto count = static_cast<size_t>(read);
#endif
    if (!count) break;
    Require(count <= limit - result.size);
    result.size += count;
    Require(crypto_hash_sha256_update(&hash, buffer.data(), count) == 0);
    if (json) result.bytes.append(reinterpret_cast<const char *>(buffer.data()), count);
  }
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
  Require(crypto_hash_sha256_final(&hash, digest.data()) == 0);
  sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
  result.hash = hex.data();
  Require(result.identity.SameObject(Identity(path)));
  return result;
}
Json Parse(const Node &node) {
  Require(!node.identity.directory);
  // Duplicate members and deep input are not supported journal/schema variants.
  std::vector<std::set<std::string>> members;
  const auto callback = [&](int depth, Json::parse_event_t event, Json &value) {
    Require(depth <= 32);
    if (event == Json::parse_event_t::object_start) members.emplace_back();
    if (event == Json::parse_event_t::key)
      Require(!members.empty() && members.back().insert(value.get<std::string>()).second);
    if (event == Json::parse_event_t::object_end) members.pop_back();
    return true;
  };
  const auto result = Json::parse(node.bytes, callback);
  Require(result.is_object());
  return result;
}

using Tree = std::map<std::string, Node>;
using View = std::map<std::string, const Node *>;
void Scan(const fs::path &directory, const std::string &prefix, Tree &tree,
          uint64_t &total, size_t depth = 0) {
  Require(depth <= 8 && Identity(directory).directory);
  const auto before = Identity(directory);
  private_io::VisitDirectory(directory, [&](const fs::path &entry) {
    const auto name = PathToUtf8(entry.filename());
    Check(WebDavTransport::ValidateRelativePath(name));
    Require(name.find('/') == std::string::npos && tree.size() < kMaxNodes);
    const auto relative = prefix.empty() ? name : prefix + '/' + name;
    auto node = ReadNode(entry, name.size() >= 5 && name.substr(name.size() - 5) == ".json");
    Require(node.size <= kMaxBytes - total);
    total += node.size;
    const bool directory_node = node.identity.directory;
    Require(tree.emplace(relative, std::move(node)).second);
    if (directory_node) Scan(entry, relative, tree, total, depth + 1);
  });
  Require(before.SameObject(Identity(directory)));
}
View Subview(const View &view, const std::string &prefix) {
  View result;
  const auto start = prefix + '/';
  for (auto it = view.lower_bound(start); it != view.end() && it->first.compare(0, start.size(), start) == 0;
       ++it)
    result.emplace(it->first.substr(start.size()), it->second);
  return result;
}
View MakeView(const Tree &tree) {
  View result;
  for (const auto &entry : tree) result.emplace(entry.first, &entry.second);
  return result;
}
std::set<std::string> Children(const View &view) {
  std::set<std::string> result;
  for (const auto &entry : view) if (entry.first.find('/') == std::string::npos) result.insert(entry.first);
  return result;
}
const Node &Get(const View &view, const std::string &name, bool directory = false) {
  const auto it = view.find(name);
  Require(it != view.end() && it->second->identity.directory == directory);
  return *it->second;
}

struct Facts {
  std::string id, url, user, baseline, retirement_operation;
  bool has_state = false, pending = false, conflicts = false, probe = false;
  bool candidate = false, retirement = false, retired = false, unreferenced = false;
  bool candidate_namespace = false, probe_namespace = false;
  WebDavStorageStatus keys;
  Json ToJson() const {
    return {{kJsonKeyVersion, 1}, {kJsonKeyNotebookId, id},
            {"provider", has_state || pending || probe_namespace || candidate || retirement || retired
                             ? Json("webdav") : Json(nullptr)},
            {kJsonKeyRemoteUrl, url.empty() ? Json(nullptr) : Json(url)},
            {"usernameHash", user.empty() ? Json(nullptr) : Json(user)},
            {"hasState", has_state}, {"pending", pending}, {"conflicts", conflicts},
            {"probe", probe}, {"credentialValidation", candidate},
            {"retirement", retirement}, {"retired", retired},
            {"retirementOperationId", retirement_operation.empty() ? Json(nullptr) : Json(retirement_operation)},
            {"cleanCloneBaseline", has_state && !pending && !conflicts && !probe && !candidate &&
                                       !retirement && !retired && !unreferenced && !candidate_namespace && !probe_namespace},
            {"baselineSha256", baseline.empty() ? Json(nullptr) : Json(baseline)}};
  }
};
void Binding(const Json &value, Facts &facts, bool published) {
  Require(value.at(kJsonKeyVersion).is_number_integer() && value.at(kJsonKeyVersion) == 1);
  const auto id = Text(value, kJsonKeyNotebookId);
  Require(Uuid(id) && (facts.id.empty() || facts.id == id));
  facts.id = id;
  const auto url = Text(value, kJsonKeyRemoteUrl);
  sync_http::Url parsed;
  Require(sync_http::ParseUrl(url, parsed) && parsed.path.back() == '/' && parsed.String() == url);
  const auto user = HashField(value, "usernameHash");
  if (published) {
    Require(facts.url.empty() || (facts.url == url && facts.user == user));
    facts.url = url;
    facts.user = user;
  }
}
void SameBinding(const Json &a, const Json &b) {
  for (const auto *key : {kJsonKeyVersion, kJsonKeyNotebookId, kJsonKeyRemoteUrl, "usernameHash"})
    Require(a.at(key) == b.at(key));
}
void Snapshots(const View &view, const std::string &operation = {}) {
  if (!view.count("snapshots")) return;
  Get(view, "snapshots", true);
  size_t files = 0;
  for (const auto &entry : Subview(view, "snapshots")) {
    const auto slash = entry.first.find('/');
    if (slash == std::string::npos) {
      Require(Uuid(entry.first) && entry.second->identity.directory &&
              (operation.empty() || operation == entry.first));
    } else {
      Require(Uuid(entry.first.substr(0, slash)) && Uuid(entry.first.substr(slash + 1)) &&
              !entry.second->identity.directory &&
              (operation.empty() || operation == entry.first.substr(0, slash)));
      if (!operation.empty()) Require(++files <= 4096 && entry.second->size <= 16 * 1024 * 1024);
    }
  }
}
void Snapshot(const View &view, const std::string &name, const std::string &hash,
              std::set<std::string> &referenced, const std::string &operation = {}) {
  Require(name.empty() == hash.empty());
  if (name.empty()) return;
  const auto parts = SplitPathComponents(name);
  Require(parts.size() == 3 && parts[0] == "snapshots" && Uuid(parts[1]) && Uuid(parts[2]) &&
          (operation.empty() || parts[1] == operation));
  Check(WebDavTransport::ValidateRelativePath(name));
  Require(Get(view, name).hash == hash);
  referenced.insert(name);
}

// This is the single read-only storage parser used by import diagnostics,
// provider transactions and encryption/reconfiguration guards. State::Open cannot
// be used here: it creates directories and has mutable recovery semantics.
Facts Layout(const View &view, const std::string &expected_id,
             const std::string &candidate_operation = {}, bool probe = false) {
  Facts facts;
  facts.id = expected_id;
  const bool candidate = !candidate_operation.empty();
  for (const auto &name : Children(view))
    Require(name == "state.json" || name == "pending.json" || name == "snapshots");
  Snapshots(view, candidate_operation);
  std::set<std::string> referenced;
  Json state;
  if (view.count("state.json")) {
    const auto &node = Get(view, "state.json");
    state = Parse(node);
    Keys(state, {kJsonKeyVersion, kJsonKeyNotebookId, kJsonKeyRemoteUrl, "usernameHash", "entries", "conflicts"});
    Binding(state, facts, true);
    facts.has_state = true;
    facts.baseline = node.hash;
    Require(state.at("entries").is_object() && state.at("conflicts").is_object() &&
            state.at("entries").size() <= kMaxNodes && state.at("conflicts").size() <= kMaxNodes);
    if (candidate || probe) Require(state.at("entries").empty() && state.at("conflicts").empty());
    for (auto it = state.at("entries").begin(); it != state.at("entries").end(); ++it) {
      LogicalPath(it.key());
      const auto kind = Text(it.value(), kJsonKeyKind);
      if (kind == "collection") Keys(it.value(), {kJsonKeyKind});
      else {
        Require(kind == "file");
        Keys(it.value(), {kJsonKeyKind, "sha256", "etag"});
        HashField(it.value(), "sha256");
        Require(WebDavTransport::IsStrongEtag(Text(it.value(), "etag")));
      }
    }
    for (auto it = state.at("conflicts").begin(); it != state.at("conflicts").end(); ++it) {
      LogicalPath(it.key());
      const auto &c = it.value();
      Keys(c, {"localSha256", "remoteSha256", "remoteEtag", "localSnapshot", "remoteSnapshot",
               kJsonKeyIsBinary, kJsonKeyCanKeepBoth, kJsonKeyLocalModifiedUtc, kJsonKeyRemoteModifiedUtc}, {"resolution"});
      const auto local = HashField(c, "localSha256", true);
      const auto remote = HashField(c, "remoteSha256", true);
      const auto etag = Text(c, "remoteEtag", true);
      Require(remote.empty() ? etag.empty() : WebDavTransport::IsStrongEtag(etag));
      Snapshot(view, Text(c, "localSnapshot", true), local, referenced);
      Snapshot(view, Text(c, "remoteSnapshot", true), remote, referenced);
      Require(c.at(kJsonKeyIsBinary).is_boolean() && c.at(kJsonKeyCanKeepBoth).is_boolean() &&
              c.at(kJsonKeyLocalModifiedUtc).is_number_integer() &&
              c.at(kJsonKeyRemoteModifiedUtc).is_number_integer());
      Require(!(webdav::IsMetadata(it.key()) || webdav::IsEncrypted(it.key())) || !c.at(kJsonKeyCanKeepBoth).get<bool>());
      if (c.contains("resolution"))
        Require(c.at("resolution") == "keep_local" || c.at("resolution") == "keep_remote" ||
                (c.at("resolution") == "keep_both" && c.at(kJsonKeyCanKeepBoth).get<bool>()));
      facts.conflicts = facts.keys.any_conflict = true;
      if (KeyPath(it.key())) facts.keys.encryption_conflict = true;
    }
  }
  if (view.count("pending.json")) {
    Require(facts.has_state);
    const auto pending = Parse(Get(view, "pending.json"));
    Keys(pending, {kJsonKeyVersion, kJsonKeyNotebookId, kJsonKeyRemoteUrl, "usernameHash", "operationId", "operations"});
    SameBinding(pending, state);
    const auto operation = Text(pending, "operationId");
    Require(Uuid(operation) && (!candidate || operation == candidate_operation) &&
            pending.at("operations").is_array() &&
            pending.at("operations").size() <= (candidate || probe ? 3 : kMaxNodes));
    std::set<std::pair<std::string, std::string>> unique;
    facts.pending = facts.keys.any_pending = true;
    for (const auto &op : pending.at("operations")) {
      Keys(op, {kJsonKeyPath, kJsonKeyKind, "action", "expectedLocalKind", "expectedRemoteKind",
                "expectedLocalSha256", "expectedRemoteEtag", "oldSha256", "newSha256",
                "sourceSnapshot", "previousSnapshot", "scratchUrl", "stage"});
      const auto path = Text(op, kJsonKeyPath);
      LogicalPath(path, true);
      const auto kind = Text(op, kJsonKeyKind), action = Text(op, "action"), stage = Text(op, "stage");
      Require(kind == "file" || kind == "collection");
      const bool upsert = action == "upsertLocal" || action == "upsertRemote";
      const bool collection = action == "mkdirLocal" || action == "mkdirRemote" ||
                              action == "removeLocalCollection";
      Require(upsert || collection || action == "deleteLocal" || action == "deleteRemote");
      Require(unique.emplace(path, action).second && (kind == "collection") == collection);
      Require(stage == "prepared" || stage == "remoteConfirmed" || stage == "localConfirmed" ||
              stage == "baselineConfirmed");
      const auto local_kind = Text(op, "expectedLocalKind"), remote_kind = Text(op, "expectedRemoteKind");
      for (const auto &k : {local_kind, remote_kind})
        Require(k == "absent" || k == "file" || k == "collection");
      const auto local = HashField(op, "expectedLocalSha256", true);
      const auto old_hash = HashField(op, "oldSha256", true), new_hash = HashField(op, "newSha256", true);
      const auto etag = Text(op, "expectedRemoteEtag", true);
      Require((local_kind == "file") == !local.empty() &&
              (remote_kind == "file" ? WebDavTransport::IsStrongEtag(etag) : etag.empty()));
      const auto source = Text(op, "sourceSnapshot", true), previous = Text(op, "previousSnapshot", true);
      const auto scratch = Text(op, "scratchUrl", true);
      Require(scratch.empty() || Scratch(scratch));
      Require(upsert ? kind == "file" && !new_hash.empty() && !source.empty()
                     : new_hash.empty() && source.empty());
      Snapshot(view, source, new_hash, referenced, probe ? operation : candidate_operation);
      Snapshot(view, previous, old_hash, referenced, probe ? operation : candidate_operation);
      if (collection)
        Require(local_kind != "file" && remote_kind != "file" && old_hash.empty() && scratch.empty());
      else {
        Require(local_kind != "collection" && remote_kind != "collection");
        if ((action == "upsertRemote" || action == "deleteRemote") && remote_kind == "file")
          Require(!previous.empty());
        Require(scratch.empty() || action == "upsertRemote");
        if (action == "upsertRemote" && !webdav::IsScratch(path)) Require(!scratch.empty());
      }
      if (candidate || probe) {
        Require(Scratch(path) && kind == "file" && action == "upsertRemote" &&
                local_kind == "absent" && remote_kind == "absent" &&
                op.at("expectedLocalSha256").is_null() && op.at("expectedRemoteEtag").is_null() &&
                op.at("scratchUrl").is_null() && (stage == "prepared" || stage == "remoteConfirmed"));
      } else {
        if (KeyPath(path)) facts.keys.encryption_pending = true;
        if (webdav::IsEncrypted(path) && (action == "upsertLocal" || action == "deleteLocal"))
          facts.keys.replaces_encryption = true;
      }
    }
  }
  for (const auto &entry : Subview(view, "snapshots"))
    if (!entry.second->identity.directory && !referenced.count("snapshots/" + entry.first))
      facts.unreferenced = true;
  // An empty candidate directory is the legitimate tail between removing its
  // binding and removing the directory. Nonempty unbound data is not that tail.
  Require(facts.has_state || view.empty() || (!candidate && Children(view) == std::set<std::string>{"snapshots"} &&
                                            !facts.unreferenced));
  return facts;
}

Json Inventory(const View &view, const std::string &name) {
  Json result = Json::array();
  const auto append = [&](const std::string &path, const Node &node) {
    result.push_back({{kJsonKeyPath, path}, {"directory", node.identity.directory},
                      {"device", node.identity.device}, {"low", node.identity.low},
                      {"high", node.identity.high}, {"size", node.size},
                      {"sha256", node.identity.directory ? Json(nullptr) : Json(node.hash)}});
  };
  const auto root = view.find(name);
  Require(root != view.end());
  append("", *root->second);
  for (const auto &entry : Subview(view, name)) append(entry.first, *entry.second);
  return result;
}
void ValidateInventory(const Json &inventory) {
  Require(inventory.is_array() && !inventory.empty() && inventory.size() <= kMaxNodes);
  std::set<std::string> paths;
  for (const auto &entry : inventory) {
    Keys(entry, {kJsonKeyPath, "directory", "device", "low", "high", "size", "sha256"});
    const auto path = Text(entry, kJsonKeyPath);
    if (!path.empty()) Check(WebDavTransport::ValidateRelativePath(path));
    Require(paths.insert(path).second && entry.at("directory").is_boolean());
    for (const auto *field : {"device", "low", "high", "size"})
      Require(entry.at(field).is_number_unsigned() ||
              (entry.at(field).is_number_integer() && entry.at(field).get<int64_t>() >= 0));
    if (entry.at("directory").get<bool>())
      Require(entry.at("size") == 0 && entry.at("sha256").is_null());
    else HashField(entry, "sha256");
  }
  Require(paths.count("") == 1 && inventory.front().at(kJsonKeyPath) == "");
}
struct Inspection {
  fs::path directory;
  Tree tree;
  Facts facts;
  Json journal;
  View live;
  std::string journal_hash;
};
// An in-memory comparison only, not a persisted seal. It detects a published
// rename/journal whose following directory flush failed, so changed never means
// "no effect" merely because the operation returned an error.
using TreeFingerprint = std::array<unsigned char, crypto_hash_sha256_BYTES>;
TreeFingerprint Fingerprint(const Tree &tree) {
  crypto_hash_sha256_state hash;
  Require(crypto_hash_sha256_init(&hash) == 0);
  const auto add = [&](const void *bytes, size_t count) {
    Require(crypto_hash_sha256_update(&hash, static_cast<const unsigned char *>(bytes), count) == 0);
  };
  for (const auto &entry : tree) {
    const uint64_t length = entry.first.size();
    add(&length, sizeof(length));
    add(entry.first.data(), entry.first.size());
    const auto &node = entry.second;
    for (const auto value : {node.identity.device, node.identity.low, node.identity.high, node.size})
      add(&value, sizeof(value));
    const uint8_t directory = node.identity.directory ? 1 : 0;
    add(&directory, sizeof(directory));
    add(node.hash.data(), node.hash.size());
  }
  TreeFingerprint result{};
  Require(crypto_hash_sha256_final(&hash, result.data()) == 0);
  return result;
}
void MergeKeys(WebDavStorageStatus &target, const WebDavStorageStatus &source) {
  target.encryption_conflict |= source.encryption_conflict;
  target.encryption_pending |= source.encryption_pending;
  target.replaces_encryption |= source.replaces_encryption;
  target.any_conflict |= source.any_conflict;
  target.any_pending |= source.any_pending;
}
View Ordinary(const View &view) {
  View result;
  for (const auto &entry : view) {
    const auto name = entry.first.substr(0, entry.first.find('/'));
    if (name == "state.json" || name == "pending.json" || name == "snapshots")
      result.emplace(entry);
  }
  return result;
}
Facts ProviderLayout(const View &view, const std::string &id, bool recovery_allowed) {
  for (const auto &name : Children(view))
    Require(name == "state.json" || name == "pending.json" || name == "snapshots" ||
            (recovery_allowed && (name == "probe" || name == "credential-validation")));
  auto facts = Layout(Ordinary(view), id);
  if (view.count("probe")) {
    Get(view, "probe", true);
    const auto probe = Layout(Subview(view, "probe"), facts.id, {}, true);
    const bool unresolved = probe.pending || probe.conflicts || probe.unreferenced;
    if (probe.has_state && facts.has_state)
      Require(probe.url == facts.url && (!unresolved || probe.user == facts.user));
    if (facts.id.empty()) facts.id = probe.id;
    facts.probe_namespace = true;
    facts.probe = unresolved;
    facts.pending |= probe.pending;
    facts.keys.any_pending |= unresolved;
    MergeKeys(facts.keys, probe.keys);
  }
  if (view.count("credential-validation")) {
    facts.candidate_namespace = true;
    Get(view, "credential-validation", true);
    const auto candidates = Subview(view, "credential-validation");
    const auto operations = Children(candidates);
    Require(operations.size() <= 4096);
    for (const auto &operation : operations) {
      Require(Uuid(operation));
      Get(candidates, operation, true);
      const auto candidate = Layout(Subview(candidates, operation), facts.id, operation);
      if (candidate.has_state) {
        Require(facts.url.empty() || candidate.url == facts.url);
        if (facts.url.empty()) facts.url = candidate.url;
        // A candidate endpoint is known, but its account is not published.
        // Do not fill usernameHash from an unpublished validation binding.
      }
      if (facts.id.empty()) facts.id = candidate.id;
      facts.candidate = facts.pending = facts.keys.any_pending = true;
    }
  }
  return facts;
}
void InspectProvider(Inspection &inspection, const std::string &id) {
  inspection.facts.id = id;
  if (!Present(inspection.directory)) return;
  Ancestors(inspection.directory);
  uint64_t total = 0;
  Scan(inspection.directory, "", inspection.tree, total);
  const auto view = MakeView(inspection.tree);
  for (const auto &name : Children(view))
    Require(name == "state.json" || name == "pending.json" || name == "snapshots" || name == "probe" ||
            name == "credential-validation" || name == "retirement.json" || name == "retired");
  View live;
  for (const auto &entry : view)
    if (entry.first != "retirement.json" && entry.first != "retired" && entry.first.compare(0, 8, "retired/") != 0)
      live.emplace(entry);
  std::string operation;
  if (view.count("retirement.json")) {
    inspection.journal = Parse(Get(view, "retirement.json"));
    inspection.journal_hash = Get(view, "retirement.json").hash;
    auto &j = inspection.journal;
    Keys(j, {kJsonKeyVersion, kJsonKeyNotebookId, "operationId", "phase", "entries"});
    Require(j.at(kJsonKeyVersion).is_number_integer() && j.at(kJsonKeyVersion) == 1);
    const auto journal_id = Text(j, kJsonKeyNotebookId);
    Require(Uuid(journal_id) && (id.empty() || id == journal_id));
    inspection.facts.id = journal_id;
    operation = Text(j, "operationId");
    const auto phase = Text(j, "phase");
    Require(Uuid(operation) && (phase == "prepared" || phase == "archived" || phase == "disabled") &&
            j.at("entries").is_array() && j.at("entries").size() <= 4);
    std::set<std::string> names;
    View combined;
    const auto archive_name = "retired/" + operation;
    if (view.count(archive_name)) Get(view, archive_name, true);
    const auto archive = Subview(view, archive_name);
    for (const auto &entry : j.at("entries")) {
      Keys(entry, {kJsonKeyName, kJsonKeyBackend}, {"inventory"});
      const auto name = Text(entry, kJsonKeyName);
      Require(entry.at(kJsonKeyBackend) == "webdav" && names.insert(name).second &&
              (name == "state.json" || name == "snapshots" || name == "pending.json" || name == "probe" ||
               name == "credential-validation"));
      const bool source = live.count(name) != 0, target = archive.count(name) != 0;
      Require(source != target);
      if (phase != "prepared") Require(target);
      const auto &location = source ? live : archive;
      if (entry.contains("inventory")) {
        ValidateInventory(entry.at("inventory"));
        Require(entry.at("inventory") == Inventory(location, name));
      }
      combined.emplace(name, location.at(name));
      for (const auto &child : Subview(location, name)) combined.emplace(name + '/' + child.first, child.second);
    }
    for (const auto &name : Children(live)) Require(names.count(name));
    for (const auto &name : Children(archive)) Require(names.count(name));
    inspection.facts = ProviderLayout(combined, journal_id, true);
    Require(!inspection.facts.candidate);
    inspection.facts.retirement = inspection.facts.keys.any_pending = true;
    inspection.live = std::move(combined);
  } else {
    inspection.facts = ProviderLayout(live, id, true);
    inspection.live = live;
  }
  if (view.count("retired")) {
    Get(view, "retired", true);
    const auto retired = Subview(view, "retired");
    const auto operations = Children(retired);
    Require(operations.size() <= 4096);
    for (const auto &archive_id : operations) {
      Require(Uuid(archive_id));
      Get(retired, archive_id, true);
      const auto archive_view = Subview(retired, archive_id);
      inspection.facts.retired = true;
      if (archive_id == operation) continue;
      const auto archived = ProviderLayout(archive_view, inspection.facts.id, true);
      // Archived key recovery still protects decryption, but old ordinary paths
      // are not active sync work. Candidates are never valid in an archive.
      Require(!archived.candidate);
      inspection.facts.keys.encryption_conflict |= archived.keys.encryption_conflict;
      inspection.facts.keys.encryption_pending |= archived.keys.encryption_pending;
    }
  }
  // Virtual XOR/candidate bindings are validation inputs, not a published
  // account or cursor. Public binding facts describe only the live state file.
  if (!live.count("state.json")) {
    inspection.facts.has_state = false;
    inspection.facts.url.clear();
    inspection.facts.user.clear();
    inspection.facts.baseline.clear();
  }
  // Expose only the identity whose complete journal, seals and archive layout
  // passed inspection. A fresh caller operation ID is never archive authority.
  inspection.facts.retirement_operation = operation;
}
Inspection InspectRoot(const std::string &root, const std::string &id) {
  Require(!root.empty() && Uuid(id), VXCORE_ERR_INVALID_PARAM);
  const auto notebook = PathFromUtf8(root);
  Ancestors(notebook);
  Ancestors(notebook / "vx_notebook");
  const auto config = Parse(ReadNode(notebook / "vx_notebook/config.json", true));
  Require(config.contains(kJsonKeyId) && config.at(kJsonKeyId).is_string() && config.at(kJsonKeyId) == id);
  const auto notebook_config = NotebookConfig::FromJson(config);
  Require(notebook_config.id == id && !notebook_config.name.empty());
  const auto sync = notebook / "vx_notebook/vx_sync";
  if (Present(sync)) {
    Ancestors(sync);
    private_io::VisitDirectory(sync, [&](const fs::path &entry) {
      Identity(entry);
      Require(entry.filename() == "webdav", VXCORE_ERR_UNSUPPORTED);
    });
  }
  Inspection result;
  result.directory = sync / "webdav";
  InspectProvider(result, id);
  return result;
}
void WriteJournal(const fs::path &path, const Json &journal, bool create) {
  const auto bytes = journal.dump();
  Require(bytes.size() <= kMaxJson);
  Ancestors(path.parent_path());
  AtomicFileWriter writer(path, FileAccess::kPrivate);
  Check(writer.Open());
  Check(writer.Write(bytes.data(), bytes.size()));
  Check(create ? writer.CommitNoReplace() : writer.Commit());
}
void CheckJournal(const Inspection &inspection, const std::string &hash) {
  const auto current = ReadNode(inspection.directory / "retirement.json", true);
  const auto expected = inspection.tree.find("retirement.json");
  Require(expected != inspection.tree.end() && current.hash == hash &&
          current.identity.SameObject(expected->second.identity));
}
void RemoveJournal(const Inspection &inspection, const std::string &hash) {
  CheckJournal(inspection, hash);
  Require(private_io::Remove(inspection.directory / "retirement.json"), VXCORE_ERR_IO);
  Check(FlushDirectory(inspection.directory, FileAccess::kPrivate));
}
void EnsureNewDirectory(const fs::path &path) {
  Ancestors(path.parent_path());
  Require(!Present(path) && private_io::MakeDirectory(path), VXCORE_ERR_IO);
  Check(FlushDirectory(path.parent_path(), FileAccess::kPrivate));
}
void Move(const fs::path &from, const fs::path &to, const Node &expected) {
  Ancestors(from.parent_path());
  Ancestors(to.parent_path());
  Require(Identity(from).SameObject(expected.identity));
  Require(!Present(to));
  Check(RenameNoReplace(from, to, FileAccess::kPrivate));
  Check(FlushDirectory(from.parent_path(), FileAccess::kPrivate));
  Check(FlushDirectory(to.parent_path(), FileAccess::kPrivate));
}
Json Result(const Inspection &inspection, const SyncProviderRuntimeFacts &runtime,
            const std::string &operation, bool changed) {
  auto result = inspection.facts.ToJson();
  result["runtimeRegistered"] = runtime.runtime_registered;
  result["applyInProgress"] = runtime.apply_in_progress;
  result["persistedEnabled"] = runtime.persisted_enabled;
  if (!operation.empty()) {
    result["operationId"] = operation;
    result["phase"] = inspection.journal.is_null() ? Json(nullptr) : inspection.journal.at("phase");
    result["changed"] = changed;
  }
  return result;
}
}  // namespace

VxCoreError InspectSyncStorage(const std::string &root, const std::string &id, Json &out) {
  out = nullptr;
  try {
    out = InspectRoot(root, id).facts.ToJson();
    return VXCORE_OK;
  } catch (const webdav::Failure &failure) { return failure.error; }
    catch (const std::bad_alloc &) { return VXCORE_ERR_OUT_OF_MEMORY; }
    catch (...) { return VXCORE_ERR_INVALID_STATE; }
}

VxCoreError InspectWebDavStorage(const fs::path &directory, WebDavStorageStatus &out) {
  out = {};
  try {
    Inspection inspection;
    inspection.directory = directory;
    InspectProvider(inspection, {});
    out = inspection.facts.keys;
    return VXCORE_OK;
  } catch (const webdav::Failure &failure) { return failure.error; }
    catch (const std::bad_alloc &) { return VXCORE_ERR_OUT_OF_MEMORY; }
    catch (...) { return VXCORE_ERR_INVALID_STATE; }
}

VxCoreError SyncProviderTransaction(const std::string &root, const std::string &id,
                                   const Json &command, const SyncProviderRuntimeFacts &runtime,
                                   Json &out) {
  out = nullptr;
  std::string operation;
  bool changed = false, have_before = false;
  TreeFingerprint before{};
  const auto reconcile = [&] {
    if (!have_before) return;
    out = nullptr;
    try {
      const auto actual = InspectRoot(root, id);
      out = Result(actual, runtime, operation, changed || before != Fingerprint(actual.tree));
    } catch (...) {
      // Unverifiable layout means unavailable facts, not an unchanged result.
    }
  };
  try {
    Require(command.is_object() && command.contains(kJsonKeyKind) && command.at(kJsonKeyKind).is_string(),
            VXCORE_ERR_INVALID_PARAM);
    const auto kind = Text(command, kJsonKeyKind);
    if (kind == "inspect") Keys(command, {kJsonKeyVersion, kJsonKeyKind});
    else {
      Keys(command, {kJsonKeyVersion, kJsonKeyKind, "operationId"});
      operation = Text(command, "operationId");
      Require(Uuid(operation), VXCORE_ERR_INVALID_PARAM);
    }
    Require(command.at(kJsonKeyVersion).is_number_integer() && command.at(kJsonKeyVersion) == 1 &&
            (kind == "inspect" || kind == "begin" || kind == "commit" || kind == "restore" || kind == "recover"),
            VXCORE_ERR_INVALID_PARAM);
    auto inspection = InspectRoot(root, id);
    out = Result(inspection, runtime, operation, false);
    if (kind == "inspect") return VXCORE_OK;
    Require(!runtime.runtime_registered && !runtime.apply_in_progress, VXCORE_ERR_SYNC_IN_PROGRESS);
    before = Fingerprint(inspection.tree);
    have_before = true;
    if (kind == "begin") {
      Require(inspection.journal.is_null() && !inspection.facts.pending && !inspection.facts.conflicts &&
              !inspection.facts.probe && !inspection.facts.candidate, VXCORE_ERR_SYNC_CONFLICT);
      // Nothing to archive is a genuine no-op; it does not create sync storage.
      if (inspection.live.empty()) return VXCORE_OK;
      Require(inspection.facts.has_state);
      const auto archive = inspection.directory / "retired" / PathFromUtf8(operation);
      Require(!Present(archive));
      Json entries = Json::array();
      for (const auto &name : Children(inspection.live))
        entries.push_back({{kJsonKeyName, name}, {kJsonKeyBackend, "webdav"},
                           {"inventory", Inventory(inspection.live, name)}});
      Json journal = {{kJsonKeyVersion, 1}, {kJsonKeyNotebookId, id}, {"operationId", operation},
                      {"phase", "prepared"}, {"entries", std::move(entries)}};
      WriteJournal(inspection.directory / "retirement.json", journal, true);
      changed = true;
      const auto retired = inspection.directory / "retired";
      if (!Present(retired)) EnsureNewDirectory(retired);
      EnsureNewDirectory(archive);
      // Reinspect the complete XOR layout, schemas and immutable identities
      // after publishing intent, before the first move.
      inspection = InspectRoot(root, id);
      for (const auto &entry : journal.at("entries")) {
        const auto name = Text(entry, kJsonKeyName);
        CheckJournal(inspection, inspection.journal_hash);
        Move(inspection.directory / PathFromUtf8(name), archive / PathFromUtf8(name),
             *inspection.live.at(name));
      }
      inspection = InspectRoot(root, id);
      CheckJournal(inspection, inspection.journal_hash);
      journal["phase"] = "archived";
      WriteJournal(inspection.directory / "retirement.json", journal, false);
    } else {
      if (kind == "commit") Require(!runtime.persisted_enabled && runtime.persisted_routing_cleared);
      if (inspection.journal.is_null()) return VXCORE_OK;
      Require(inspection.journal.at("operationId") == operation);
      // Desktop v1 records without immutable ownership inventories remain
      // readable recovery facts, never mutation authority. Snapshotting their
      // current bytes would falsely bless a replacement as the original.
      Require(!inspection.journal.at("entries").empty());
      for (const auto &entry : inspection.journal.at("entries"))
        Require(entry.contains("inventory"));
      const auto phase = Text(inspection.journal, "phase");
      const bool commit = kind == "commit" || (kind == "recover" && phase == "disabled");
      if (commit) {
        Require(!runtime.persisted_enabled && runtime.persisted_routing_cleared);
        Require(phase == "archived" || phase == "disabled");
        if (phase != "disabled") {
          auto journal = inspection.journal;
          journal["phase"] = "disabled";
          CheckJournal(inspection, inspection.journal_hash);
          WriteJournal(inspection.directory / "retirement.json", journal, false);
          changed = true;
          inspection = InspectRoot(root, id);
        }
        RemoveJournal(inspection, inspection.journal_hash);
        changed = true;
      } else {
        Require(phase == "prepared" || phase == "archived");
        // Publish the restoring phase BEFORE moving anything. A crash during
        // restore must remain a valid prepared XOR layout.
        if (phase == "archived") {
          auto journal = inspection.journal;
          journal["phase"] = "prepared";
          CheckJournal(inspection, inspection.journal_hash);
          WriteJournal(inspection.directory / "retirement.json", journal, false);
          changed = true;
          inspection = InspectRoot(root, id);
        }
        const auto archive = inspection.directory / "retired" / PathFromUtf8(operation);
        // InspectRoot validated ALL entries, source XOR archive, exact hashes,
        // identities and closed schemas before any restore can take effect.
        for (const auto &entry : inspection.journal.at("entries")) {
          const auto name = Text(entry, kJsonKeyName);
          const auto source = inspection.directory / PathFromUtf8(name);
          const auto target = archive / PathFromUtf8(name);
          if (Present(target)) {
            CheckJournal(inspection, inspection.journal_hash);
            Move(target, source, *inspection.live.at(name));
            changed = true;
          }
        }
        inspection = InspectRoot(root, id);
        RemoveJournal(inspection, inspection.journal_hash);
        changed = true;
      }
    }
    out = Result(InspectRoot(root, id), runtime, operation, changed);
    return VXCORE_OK;
  } catch (const webdav::Failure &failure) {
    reconcile();
    return failure.error;
  } catch (const std::bad_alloc &) {
    out = nullptr;
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const fs::filesystem_error &) {
    reconcile();
    return VXCORE_ERR_IO;
  } catch (...) {
    reconcile();
    return VXCORE_ERR_INVALID_STATE;
  }
}

}  // namespace vxcore
