#include "jianguoyun_sync_backend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <utility>

#include "core/folder.h"
#include "core/notebook.h"
#include "jianguoyun_state.h"
#include "jianguoyun_transport.h"
#include "sync/credential_provider.h"
#include "sync/sync_json_keys.h"
#include "sync/webdav/webdav_state.h"
#include "sync/webdav/webdav_transport.h"
#include "utils/file_utils.h"
#include "vxcore/notebook_json_keys.h"

namespace vxcore {
namespace fs = std::filesystem;
using namespace jianguoyun;
using Json = nlohmann::json;
using webdav::Check;
using webdav::CheckCancelled;
using webdav::Failure;
using webdav::HashBytes;
using webdav::HashFile;
using webdav::NewId;
using webdav::ReadBytes;
using webdav::Require;
namespace {

const char *Diagnostic(VxCoreError error) {
  switch (error) {
    case VXCORE_OK:
      return "";
    case VXCORE_ERR_CANCELLED:
      return "Jianguoyun synchronization was cancelled. Recovery data was retained.";
    case VXCORE_ERR_SYNC_AUTH_FAILED:
      return "Jianguoyun authentication failed. Check the username and app password.";
    case VXCORE_ERR_SYNC_NETWORK:
      return "The Jianguoyun request failed. Check the connection and TLS certificate, then retry.";
    case VXCORE_ERR_SYNC_CONFLICT:
      return "Jianguoyun found concurrent revisions. Local data and recovery snapshots were "
             "retained.";
    case VXCORE_ERR_SYNC_IN_PROGRESS:
      return "The notebook changed or is busy. Retry when current work finishes.";
    case VXCORE_ERR_NOT_FOUND:
      return "The dedicated Jianguoyun collection or a required immutable object was not found.";
    case VXCORE_ERR_UNSUPPORTED:
      return "Jianguoyun requires safe paths, managed storage, raw-token CAS and create-only MOVE.";
    case VXCORE_ERR_PERMISSION_DENIED:
      return "The Jianguoyun account does not have permission for this operation.";
    case VXCORE_ERR_ENCRYPTION_FORMAT:
      return "The synchronized encryption cohort is invalid. Existing data was preserved.";
    case VXCORE_ERR_IO:
      return "Jianguoyun could not safely read or publish local recovery data. Check local "
             "storage.";
    case VXCORE_ERR_INVALID_PARAM:
      return "The Jianguoyun settings or notebook path are invalid.";
    default:
      return "Jianguoyun identity, history, metadata or recovery state is invalid. Existing data "
             "was preserved.";
  }
}
bool SameHead(const Head &a, const Head &b) {
  return a.repository_id == b.repository_id && a.notebook_id == b.notebook_id &&
         a.generation == b.generation && a.commit_hash == b.commit_hash;
}
bool SameTree(const Tree &a, const Tree &b) {
  if (a.size() != b.size()) return false;
  auto l = a.begin();
  auto r = b.begin();
  for (; l != a.end(); ++l, ++r)
    if (l->first != r->first || !SameRevision(l->second, r->second)) return false;
  return true;
}
bool RawSame(const Version &a, const Version &b) {
  return Same(a.entry, b.entry) && (a.entry.kind != "file" || a.raw_sha256 == b.raw_sha256);
}
bool Protected(const std::string &path, const std::vector<std::string> &paths) {
  for (const auto &p : paths)
    if (path == p || path.compare(0, p.size() + 1, p + "/") == 0 ||
        p.compare(0, path.size() + 1, path + "/") == 0)
      return true;
  return false;
}
Entry Tombstone() {
  Entry e;
  e.kind = "tombstone";
  e.revision = NewId();
  return e;
}
struct Remote {
  std::optional<Head> head;
  std::string token;
  std::string bytes;
  Commit commit;
};
}  // namespace

struct JianguoyunSyncBackend::Impl {
  explicit Impl(const SyncConfig &cfg, std::shared_ptr<ICredentialProvider> credentials)
      : config(cfg), provider(std::move(credentials)) {}
  SyncConfig config;
  std::shared_ptr<ICredentialProvider> provider;
  std::shared_ptr<ICredentialProvider> active_provider;
  SyncCancellationPtr cancellation;
  SyncCancellationPtr token;
  mutable std::mutex mutex;
  std::atomic<bool> busy{false};
  std::atomic<bool> initialized{false};
  State state;
  Versions local;
  std::vector<SyncFileInfo> status;
  std::vector<SyncConflictInfo> public_conflicts;
  std::string last_error;
  bool prepared = false;
  bool exchanged = false;
  SyncProgressCallback callback;
  void *userdata = nullptr;

  template <typename Function>
  VxCoreError Run(Function function) {
    if (busy.exchange(true)) return VXCORE_ERR_SYNC_IN_PROGRESS;
    struct Release {
      std::atomic<bool> &flag;
      ~Release() { flag.store(false); }
    } release{busy};
    {
      std::lock_guard<std::mutex> lock(mutex);
      token = cancellation;
      last_error.clear();
    }
    state.cancellation = token;
    VxCoreError result = VXCORE_OK;
    try {
      CheckCancelled(token);
      function();
    } catch (const Failure &failure) {
      result = failure.error;
    } catch (const fs::filesystem_error &) {
      result = VXCORE_ERR_IO;
    } catch (const Json::exception &) {
      result = VXCORE_ERR_INVALID_STATE;
    } catch (const std::exception &) {
      result = VXCORE_ERR_INVALID_STATE;
    } catch (...) {
      result = VXCORE_ERR_UNKNOWN;
    }
    std::vector<SyncConflictInfo> conflicts;
    for (const auto &item : state.conflicts) {
      const auto &c = item.second;
      conflicts.push_back({item.first, c.local.modified_utc, c.remote.modified_utc,
                           c.local.binary || c.remote.binary, c.can_keep_both});
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      public_conflicts.swap(conflicts);
      if (last_error.empty()) last_error = Diagnostic(result);
    }
    token.reset();
    state.cancellation.reset();
    active_provider.reset();
    return result;
  }
  void Progress(SyncState phase, const char *message, float percentage) {
    if (callback) callback({message, percentage, phase}, userdata);
  }
  void Net(JianguoyunTransport &transport, VxCoreError error) {
    if (error == VXCORE_OK) return;
    {
      std::lock_guard<std::mutex> lock(mutex);
      last_error = transport.LastError();
    }
    Check(error);
  }
  std::unique_ptr<JianguoyunTransport> Session(std::string &username) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      active_provider = provider;
    }
    Require(active_provider != nullptr, VXCORE_ERR_INVALID_PARAM);
    SyncCredentials credentials;
    Require(active_provider->GetCredentials(config.remote_url, "", &credentials),
            VXCORE_ERR_SYNC_AUTH_FAILED);
    Require(credentials.extra.is_object() && credentials.extra.contains(kJsonKeyUsername) &&
                credentials.extra.at(kJsonKeyUsername).is_string() &&
                !credentials.extra.at(kJsonKeyUsername).get_ref<const std::string &>().empty() &&
                credentials.extra.contains(kJsonKeyPassword) &&
                credentials.extra.at(kJsonKeyPassword).is_string() &&
                !credentials.extra.at(kJsonKeyPassword).get_ref<const std::string &>().empty(),
            VXCORE_ERR_SYNC_AUTH_FAILED);
    username = HashBytes(credentials.extra.at(kJsonKeyUsername).get<std::string>());
    auto transport = std::make_unique<JianguoyunTransport>(config.remote_url, credentials, token);
    Net(*transport, transport->Initialize());
    transport->SetProgressCallback([this](uint64_t bytes, uint64_t total) {
      Progress(SyncState::kFetching, "Transferring managed Jianguoyun data",
               total ? static_cast<float>(100.0 * static_cast<double>(bytes) / total) : 0.0f);
    });
    return transport;
  }
  Commit ReadCommit(JianguoyunTransport &transport, const Head &head) {
    std::string bytes, tag;
    Net(transport, transport.Get(CommitPath(head.commit_hash), bytes, tag, kManifestBytes));
    auto commit = DecodeCommit(bytes, head.commit_hash);
    Require(SameHead(commit.identity, head));
    return commit;
  }
  Remote ReadRemote(JianguoyunTransport &transport, bool allow_absent) {
    Remote result;
    const auto error = transport.Get(kHeadPath, result.bytes, result.token, 4096);
    if (error == VXCORE_ERR_NOT_FOUND) {
      Require(allow_absent && state.repository_id.empty(), VXCORE_ERR_INVALID_STATE);
      // A known ordinary notebook must never be silently converted to managed storage.
      JianguoyunResource ordinary;
      const auto ordinary_error = transport.Stat("vx_notebook", ordinary);
      Require(ordinary_error == VXCORE_ERR_NOT_FOUND,
              ordinary_error == VXCORE_OK ? VXCORE_ERR_UNSUPPORTED : ordinary_error);
      return result;
    }
    Net(transport, error);
    result.head = DecodeHead(ParseRecord(result.bytes, 4096));
    Require(JianguoyunTransport::IsRawEtag(result.token));
    Require(state.notebook_id.empty() || result.head->notebook_id == state.notebook_id);
    Require(state.repository_id.empty() || result.head->repository_id == state.repository_id);
    result.commit = ReadCommit(transport, *result.head);
    return result;
  }
  bool Descends(JianguoyunTransport &transport, const Remote &remote, const Head &ancestor) {
    Require(remote.head && remote.head->notebook_id == ancestor.notebook_id &&
            remote.head->repository_id == ancestor.repository_id &&
            remote.head->generation >= ancestor.generation);
    Commit current = remote.commit;
    for (size_t count = 0; count < kMaxAncestry; ++count) {
      if (current.identity.generation == ancestor.generation)
        return current.identity.commit_hash == ancestor.commit_hash;
      Require(!current.parent.empty());
      Head parent{current.identity.repository_id, current.identity.notebook_id,
                  current.identity.generation - 1, current.parent};
      current = ReadCommit(transport, parent);
    }
    throw Failure{VXCORE_ERR_INVALID_STATE};
  }
  void ValidateHistory(JianguoyunTransport &transport, const Remote &remote) {
    if (!state.head) return;
    Require(remote.head && Descends(transport, remote, *state.head));
  }
  void EnsureStore(JianguoyunTransport &transport) {
    for (const char *path :
         {".vnote-sync", ".vnote-sync/staging", ".vnote-sync/objects", ".vnote-sync/commits"})
      Net(transport, transport.EnsureCollection(path));
  }
  void Probe(JianguoyunTransport &transport) {
    const auto operation = NewId();
    const auto a = StagingPath(operation), b = StagingPath(operation);
    const auto c = StagingPath(operation), d = StagingPath(operation);
    const auto bytes_a = NewId() + NewId(), bytes_b = NewId() + NewId();
    const auto snapshot_a = state.SnapshotBytes(bytes_a), snapshot_b = state.SnapshotBytes(bytes_b);
    auto intent = state.Binding();
    intent["operationId"] = operation;
    intent[kJsonKeyResources] = Json::object();
    for (const auto &path : {a, c})
      intent[kJsonKeyResources][path] = {
          {"snapshot", snapshot_a}, {"sha256", HashBytes(bytes_a)}, {"size", bytes_a.size()}};
    for (const auto &path : {b, d})
      intent[kJsonKeyResources][path] = {
          {"snapshot", snapshot_b}, {"sha256", HashBytes(bytes_b)}, {"size", bytes_b.size()}};
    Check(WriteFileAtomic(state.Resolve("probe.json"), intent.dump()));
    Net(transport, transport.EnsureCollection(".vnote-sync"));
    Net(transport, transport.EnsureCollection(".vnote-sync/staging"));
    Net(transport, transport.EnsureCollection(".vnote-sync/staging/" + operation));
    Net(transport, transport.PutBytes(a, bytes_a));
    Net(transport, transport.PutBytes(b, bytes_b));
    std::string body, old_tag, tag;
    Net(transport, transport.Get(a, body, old_tag, 1024));
    Require(body == bytes_a && JianguoyunTransport::IsRawEtag(old_tag), VXCORE_ERR_UNSUPPORTED);
    const auto bad = transport.PutBytes(a, bytes_b, "vnote-invalid-" + NewId());
    Require(bad == VXCORE_ERR_SYNC_CONFLICT && transport.LastResponse().http_status == 412,
            VXCORE_ERR_UNSUPPORTED);
    Net(transport, transport.Get(a, body, tag, 1024));
    Require(body == bytes_a && tag == old_tag, VXCORE_ERR_UNSUPPORTED);
    Net(transport, transport.PutBytes(a, bytes_b, old_tag));
    Net(transport, transport.Get(a, body, tag, 1024));
    Require(body == bytes_b && tag != old_tag && JianguoyunTransport::IsRawEtag(tag),
            VXCORE_ERR_UNSUPPORTED);
    const auto stale = transport.PutBytes(a, bytes_a, old_tag);
    Require(stale == VXCORE_ERR_SYNC_CONFLICT && transport.LastResponse().http_status == 412,
            VXCORE_ERR_UNSUPPORTED);
    Net(transport, transport.Get(a, body, tag, 1024));
    Require(body == bytes_b, VXCORE_ERR_UNSUPPORTED);
    Net(transport, transport.PutBytes(c, bytes_a));
    Net(transport, transport.MoveCreate(c, d));
    Net(transport, transport.Get(d, body, tag, 1024));
    Require(body == bytes_a, VXCORE_ERR_UNSUPPORTED);
    const auto occupied = transport.MoveCreate(b, d);
    Require(occupied == VXCORE_ERR_SYNC_CONFLICT, VXCORE_ERR_UNSUPPORTED);
    Net(transport, transport.Get(d, body, tag, 1024));
    Require(body == bytes_a, VXCORE_ERR_UNSUPPORTED);
    Net(transport, transport.Get(b, body, tag, 1024));
    Require(body == bytes_b, VXCORE_ERR_UNSUPPORTED);
    // Only owned staging resources were touched. They are retained; no DELETE semantics
    // or collection-creation status is used as ownership or concurrency evidence.
    Require(fs::remove(state.Resolve("probe.json")), VXCORE_ERR_IO);
    state.CollectSnapshots();
  }
  void Initialize(const std::string &root_name, const SyncConfig &cfg) {
    initialized.store(false);
    prepared = exchanged = false;
    config = cfg;
    Require(config.backend == "jianguoyun", VXCORE_ERR_INVALID_PARAM);
    state = State{};
    state.cancellation = token;
    local.clear();
    const auto root = fs::absolute(PathFromUtf8(root_name));
    Require(fs::is_directory(root) && CheckReparsePoint(PathToUtf8(root)) == ReparseState::kNo,
            VXCORE_ERR_UNSUPPORTED);
    fs::path file;
    Check(WebDavTransport::ResolveLocalPath(root, kConfigPath, file));
    const auto notebook = webdav::NotebookJson(file);
    std::string username;
    auto transport = Session(username);
    state.Open(root, notebook.at(kJsonKeyId).get<std::string>(), transport->CanonicalRoot(),
               username);
    JianguoyunResource resource;
    Net(*transport, transport->Stat("", resource));
    Require(resource.collection, VXCORE_ERR_UNSUPPORTED);
    const auto remote = ReadRemote(*transport, true);
    ValidateHistory(*transport, remote);
    if (state.pending || !state.conflicts.empty())
      Require(state.username_hash == username, VXCORE_ERR_SYNC_IN_PROGRESS);
    Probe(*transport);
    // Commit credential/binding changes only after all authentication and probes succeed.
    // Empty comparison entries still produce union; the observed head prevents recreation
    // between setup and first sync from being mistaken for a new repository.
    if (!state.pending && state.conflicts.empty()) state.RotateUsername(username);
    if (remote.head && !state.pending) {
      state.repository_id = remote.head->repository_id;
      state.head = remote.head;
      state.Save();
    }
    initialized.store(true);
  }
  Version ReadLocal(const std::string &path, bool snapshot) {
    const auto raw = webdav::ReadLocal(state.root, path, state.notebook_id, token);
    Version result;
    result.entry.kind = raw.kind == "collection" ? "directory" : raw.kind;
    result.entry.sha256 = raw.sha256;
    result.raw_sha256 = raw.raw_sha256;
    result.modified_utc = raw.modified_utc;
    result.binary = raw.binary;
    if (result.entry.kind == "file") {
      fs::path file;
      Check(WebDavTransport::ResolveLocalPath(state.root, path, file));
      std::string projected;
      if (path == kConfigPath) {
        projected = webdav::ProjectConfig(webdav::NotebookJson(file, state.notebook_id));
        result.entry.size = projected.size();
      } else
        result.entry.size = fs::file_size(file);
      Version previous;
      if (state.pending) previous = Lookup(state.pending->local, path);
      const auto conflict = state.conflicts.find(path);
      if (!Same(result.entry, previous.entry) && conflict != state.conflicts.end())
        previous = conflict->second.local;
      const auto base = Lookup(state.entries, path);
      if (Same(result.entry, previous.entry)) {
        result.entry = previous.entry;
        if (snapshot && !previous.snapshot.empty()) {
          state.VerifySnapshot(previous.snapshot, result.entry.sha256, result.entry.size);
          result.snapshot = previous.snapshot;
        }
      } else if (Same(result.entry, base))
        result.entry = base;
      else
        result.entry.revision = NewId();
      if (snapshot && result.snapshot.empty()) {
        result.snapshot =
            path == kConfigPath ? state.SnapshotBytes(projected) : state.SnapshotFile(file);
        state.VerifySnapshot(result.snapshot, result.entry.sha256, result.entry.size);
      }
      if (snapshot && result.entry.chunks.empty() && result.entry.size)
        result.entry.chunks = HashChunks(state.Resolve(result.snapshot), token);
      const auto after = webdav::ReadLocal(state.root, path, state.notebook_id, token);
      Require(after.raw_sha256 == result.raw_sha256 && after.sha256 == result.entry.sha256,
              VXCORE_ERR_SYNC_IN_PROGRESS);
    } else if (result.entry.kind == "directory") {
      const auto base = Lookup(state.entries, path);
      const auto previous = state.pending ? Lookup(state.pending->local, path).entry : Entry{};
      result.entry.revision = Same(result.entry, previous) ? previous.revision
                              : Same(result.entry, base)   ? base.revision
                                                           : NewId();
    }
    return result;
  }
  Versions Scan(bool snapshots, bool require_config = true) {
    Versions result;
    fs::recursive_directory_iterator it(state.root), end;
    for (; it != end; ++it) {
      CheckCancelled(token);
      Require(it.depth() < static_cast<int>(kMaxDepth) && result.size() < kMaxEntries,
              VXCORE_ERR_UNSUPPORTED);
      const auto path = PathToGenericUtf8(it->path().lexically_relative(state.root));
      Check(WebDavTransport::ValidateRelativePath(path));
      if (webdav::IsExcluded(path, config)) {
        if (it->is_directory()) it.disable_recursion_pending();
        continue;
      }
      ValidatePath(path);
      Require(CheckReparsePoint(PathToUtf8(it->path())) == ReparseState::kNo,
              VXCORE_ERR_UNSUPPORTED);
      result.emplace(path, ReadLocal(path, snapshots));
    }
    ValidateTree(Entries(result));
    Require(!require_config || Lookup(result, kConfigPath).entry.kind == "file");
    return result;
  }
  fs::path Payload(const std::string &path, const Version &version, bool allow_local) {
    if (!version.snapshot.empty()) {
      state.VerifySnapshot(version.snapshot, version.entry.sha256, version.entry.size);
      return state.Resolve(version.snapshot);
    }
    Require(allow_local);
    const auto actual = ReadLocal(path, false);
    Require(Same(actual.entry, version.entry), VXCORE_ERR_SYNC_IN_PROGRESS);
    fs::path file;
    Check(WebDavTransport::ResolveLocalPath(state.root, path, file));
    return file;
  }
  void ValidateCohort(const Versions &tree, bool allow_local) {
    ValidateTree(Entries(tree));
    Require(Lookup(tree, kConfigPath).entry.kind == "file");
    std::vector<std::string> ids;
    std::optional<NotebookEncryption::KeyEnvelope> envelope;
    const auto encryption = tree.find(kEncryptionPath);
    if (encryption != tree.end() && Present(encryption->second.entry)) {
      Require(encryption->second.entry.kind == "file", VXCORE_ERR_ENCRYPTION_FORMAT);
      NotebookEncryption::KeyEnvelope key;
      Check(NotebookEncryption::ReadKeyEnvelope(
          Payload(encryption->first, encryption->second, allow_local), key));
      Require(key.notebook_id == state.notebook_id, VXCORE_ERR_ENCRYPTION_FORMAT);
      envelope = key;
    }
    const auto config_file = Payload(kConfigPath, tree.at(kConfigPath), allow_local);
    const auto notebook_json = webdav::NotebookJson(config_file, state.notebook_id);
    const auto notebook = NotebookConfig::FromJson(notebook_json);
    Require(!notebook.encryption_initialized ||
                *notebook.encryption_initialized == envelope.has_value(),
            VXCORE_ERR_ENCRYPTION_FORMAT);
    std::set<std::string> encrypted_notes;
    for (const auto &item : tree) {
      if (item.second.entry.kind != "file" || !webdav::IsMetadata(item.first)) continue;
      const auto file = Payload(item.first, item.second, allow_local);
      webdav::ValidateMetadata(item.first, file, state.notebook_id, &ids);
      if (item.first == kConfigPath) continue;
      const auto folder = FolderConfig::FromJson(Json::parse(ReadBytes(file)));
      const auto relative = item.first.substr(std::string("vx_notebook/contents/").size());
      const auto parent = PathFromUtf8(relative).parent_path();
      for (const auto &record : folder.files) {
        const auto path = PathToGenericUtf8(parent / PathFromUtf8(record.name));
        Require(Lookup(tree, path).entry.kind == "file");
        if (record.CheckProtectionMetadata() == VXCORE_ERR_ENCRYPTION_LOCKED)
          encrypted_notes.insert(path);
      }
      for (const auto &name : folder.folders) {
        const auto path = PathToGenericUtf8(parent / PathFromUtf8(name));
        Require(Lookup(tree, path).entry.kind == "directory" &&
                Lookup(tree, "vx_notebook/contents/" + path + "/vx.json").entry.kind == "file");
      }
    }
    std::set<std::string> unique;
    for (const auto &id : ids) Require(unique.insert(id).second);
    for (const auto &item : tree) {
      if (item.second.entry.kind != "file" || item.first == kEncryptionPath ||
          !webdav::IsEncrypted(item.first))
        continue;
      Require(envelope.has_value(), VXCORE_ERR_ENCRYPTION_FORMAT);
      NotebookEncryption::ObjectHeader header;
      Check(NotebookEncryption::ReadObjectHeader(Payload(item.first, item.second, allow_local),
                                                 header));
      Require(header.notebook_key_id == envelope->notebook_key_id, VXCORE_ERR_ENCRYPTION_FORMAT);
      // FileRecord::id and the encryption document UUID are distinct identities.
      // ReadObjectHeader validates its own UUID; the envelope owns key membership.
      if (encrypted_notes.count(item.first))
        Require(header.kind == "note", VXCORE_ERR_ENCRYPTION_FORMAT);
    }
  }
  bool ValidateInterruptedCohort(const Versions &current) {
    if (!state.pending || state.pending->phase != "applying") return false;
    const auto &operations = state.pending->apply;
    bool installed = false;
    for (const auto &item : operations) {
      const auto actual = Lookup(current, item.first);
      const bool expected = RawSame(actual, item.second.expected);
      const bool intended = Same(actual.entry, item.second.desired.entry);
      if (!expected && !intended) return false;
      installed = installed || (!expected && intended);
    }
    if (!installed) return false;
    // A committed file replacement can precede its sibling key/metadata replacement.
    // Validate the journal's completed cohort without pretending those bytes are already
    // installed. Analyze/Apply still use the actual fingerprints and GUI reservation.
    auto completed = current;
    for (const auto &item : operations) completed[item.first] = item.second.desired;
    ValidateCohort(completed, false);
    return true;
  }
  void Stage() {
    Require(initialized.load(), VXCORE_ERR_INVALID_STATE);
    prepared = exchanged = false;
    Progress(SyncState::kStaging, "Snapshotting local Jianguoyun notebook", 0);
    local = Scan(true);
    if (!ValidateInterruptedCohort(local)) ValidateCohort(local, false);
    // An in-flight publication keeps its original fingerprints until the next verified
    // head read resolves its outcome. New local work is held separately in this snapshot.
    prepared = true;
  }
  void BeginPending() {
    if (state.pending) return;
    state.pending = Pending{};
    state.pending->operation_id = NewId();
    state.pending->fresh_bootstrap = state.repository_id.empty();
    state.pending->local = local;
    state.SavePending();
  }
  void EnsureChunk(JianguoyunTransport &transport, const Chunk &chunk) {
    auto &uploads = state.pending->uploads;
    const auto canonical = ObjectPath(chunk.sha256);
    auto existing = uploads.find(canonical);
    if (existing != uploads.end() && existing->second.stage == "publishedVerified") return;
    const auto snapshot = state.AllocateSnapshot();
    Net(transport,
        transport.Download(canonical, state.directory, snapshot, chunk.sha256, chunk.size));
    Upload verified;
    verified.canonical = canonical;
    verified.staging = StagingPath(state.pending->operation_id);
    verified.snapshot = snapshot;
    verified.sha256 = chunk.sha256;
    verified.size = chunk.size;
    verified.stage = "publishedVerified";
    uploads[canonical] = std::move(verified);
    state.SavePending();
  }
  Version Download(JianguoyunTransport &transport, const std::string &path, const Entry &entry) {
    Version result;
    result.entry = entry;
    if (entry.kind != "file") return result;
    const auto current = Lookup(local, path);
    const auto saved = state.pending ? Lookup(state.pending->target, path) : Version{};
    for (const auto *candidate : {&current, &saved}) {
      if (Same(candidate->entry, entry) && !candidate->snapshot.empty()) {
        state.VerifySnapshot(candidate->snapshot, entry.sha256, entry.size);
        result.snapshot = candidate->snapshot;
        result.raw_sha256 = entry.sha256;
        result.binary = candidate->binary;
        return result;
      }
    }
    BeginPending();
    result.snapshot = state.AllocateSnapshot();
    AtomicFileWriter writer(state.Resolve(result.snapshot));
    Check(writer.Open());
    std::array<char, 64 * 1024> bytes{};
    for (const auto &chunk : entry.chunks) {
      EnsureChunk(transport, chunk);
      const auto &cached = state.pending->uploads.at(ObjectPath(chunk.sha256));
      std::ifstream input(state.Resolve(cached.snapshot), std::ios::binary);
      Require(input.good(), VXCORE_ERR_IO);
      input.seekg(static_cast<std::streamoff>(cached.offset));
      uint64_t remaining = chunk.size;
      while (remaining) {
        CheckCancelled(token);
        const auto count = static_cast<size_t>(std::min<uint64_t>(remaining, bytes.size()));
        input.read(bytes.data(), static_cast<std::streamsize>(count));
        Require(input.gcount() == static_cast<std::streamsize>(count) && !input.bad(),
                VXCORE_ERR_IO);
        Check(writer.Write(bytes.data(), count));
        remaining -= count;
      }
    }
    Check(writer.Commit());
    state.VerifySnapshot(result.snapshot, entry.sha256, entry.size);
    result.raw_sha256 = entry.sha256;
    webdav::ValidateMetadata(path, state.Resolve(result.snapshot), state.notebook_id);
    if (path == kConfigPath)
      Require(webdav::ProjectConfig(
                  webdav::NotebookJson(state.Resolve(result.snapshot), state.notebook_id)) ==
              ReadBytes(state.Resolve(result.snapshot)));
    std::ifstream input(state.Resolve(result.snapshot), std::ios::binary);
    std::array<char, 8192> sample{};
    input.read(sample.data(), static_cast<std::streamsize>(sample.size()));
    result.binary = webdav::IsEncrypted(path) ||
                    std::find(sample.begin(), sample.begin() + input.gcount(), '\0') !=
                        sample.begin() + input.gcount();
    return result;
  }
  Versions Materialize(JianguoyunTransport &transport, const Tree &entries) {
    Versions result;
    for (const auto &item : entries) {
      CheckCancelled(token);
      result.emplace(item.first, Download(transport, item.first, item.second));
    }
    if (!entries.empty()) ValidateCohort(result, false);
    return result;
  }
  void ConflictAt(const std::string &path, const Version &l, const Version &r) {
    Conflict conflict;
    conflict.local = l;
    conflict.remote = r;
    conflict.can_keep_both = l.entry.kind == "file" && r.entry.kind == "file" &&
                             !webdav::IsMetadata(path) && !webdav::IsEncrypted(path);
    Require(l.entry.kind != "file" || !l.snapshot.empty());
    Require(r.entry.kind != "file" || !r.snapshot.empty());
    state.conflicts[path] = std::move(conflict);
  }
  std::string ConflictName(const std::string &path, const Versions &remote) {
    const auto p = PathFromUtf8(path);
    const auto name =
        PathToUtf8(p.stem()) + ".sync-conflict-" + NewId() + PathToUtf8(p.extension());
    const auto candidate = PathToGenericUtf8(p.parent_path() / PathFromUtf8(name));
    ValidatePath(candidate);
    Require(!local.count(candidate) && !remote.count(candidate) && !state.entries.count(candidate));
    return candidate;
  }
  Versions Analyze(const Versions &remote) {
    Progress(SyncState::kAnalyzing, "Comparing managed Jianguoyun revisions", 0);
    std::set<std::string> paths;
    for (const auto &item : local) paths.insert(item.first);
    for (const auto &item : remote) paths.insert(item.first);
    for (const auto &item : state.entries) paths.insert(item.first);
    for (const auto &item : state.conflicts) paths.insert(item.first);
    Versions target = remote;
    std::vector<SyncFileInfo> view;
    bool conflict_found = false;
    for (const auto &path : paths) {
      CheckCancelled(token);
      if (webdav::IsExcluded(path, config)) continue;
      auto l = Lookup(local, path);
      auto r = Lookup(remote, path);
      if (Present(l.entry) && Present(r.entry) && l.entry.kind != r.entry.kind) {
        std::lock_guard<std::mutex> lock(mutex);
        last_error = "A file and folder use the same path. Rename one of them, then sync again.";
        throw Failure{VXCORE_ERR_UNSUPPORTED};
      }
      const auto b = Lookup(state.entries, path);
      if (!Present(l.entry) && Present(b)) l.entry = Tombstone();
      if (!Present(l.entry) && !Present(b) && b.kind == "tombstone") l.entry = b;
      auto choose = [&](Version desired) {
        if (!Present(desired.entry)) {
          if (r.entry.kind == "tombstone")
            desired.entry = r.entry;
          else if (desired.entry.kind == "absent")
            desired.entry = Tombstone();
        }
        target[path] = std::move(desired);
      };
      auto incident = state.conflicts.find(path);
      if (Same(l.entry, r.entry)) {
        if (Present(r.entry) || r.entry.kind == "tombstone")
          choose(r);
        else
          target.erase(path);
        // Choices are retained until Apply acknowledges the version.
        continue;
      }
      if (incident != state.conflicts.end()) {
        const auto &choice = incident->second;
        const bool valid =
            Same(l.entry, choice.local.entry) && SameRevision(r.entry, choice.remote.entry);
        if (!valid || choice.resolution.empty()) {
          ConflictAt(path, l, r);
          conflict_found = true;
          view.push_back({path, SyncFileStatus::kConflicted});
          continue;
        }
        if (choice.resolution == "keep_remote")
          choose(r);
        else {
          choose(l);
          if (choice.resolution == "keep_both") {
            Require(choice.can_keep_both);
            auto copy = r;
            copy.entry.revision = NewId();
            target.emplace(ConflictName(path, remote), std::move(copy));
          }
        }
        continue;
      }
      // Missing baseline is a non-destructive union. A tombstone learned for the first
      // time cannot erase an unbased local file.
      if (b.kind == "absent" && !Present(r.entry) && Present(l.entry)) {
        choose(l);
        view.push_back({path, SyncFileStatus::kAddedLocal});
      } else if (b.kind == "absent" && !Present(l.entry) && Present(r.entry)) {
        choose(r);
        view.push_back({path, SyncFileStatus::kAddedRemote});
      } else if (Same(l.entry, b)) {
        choose(r);
        view.push_back({path, Present(r.entry) ? SyncFileStatus::kModifiedRemote
                                               : SyncFileStatus::kDeletedRemote});
      } else if (Same(r.entry, b)) {
        choose(l);
        view.push_back({path, Present(l.entry) ? SyncFileStatus::kModifiedLocal
                                               : SyncFileStatus::kDeletedLocal});
      } else {
        ConflictAt(path, l, r);
        conflict_found = true;
        view.push_back({path, SyncFileStatus::kConflicted});
      }
    }
    // An independently edited descendant prevents deletion/type replacement of its
    // container. Record the container conflict, rather than dropping that child.
    for (const auto &item : target) {
      if (!Present(item.second.entry)) continue;
      auto parent = PathFromUtf8(item.first).parent_path();
      while (!parent.empty()) {
        const auto path = PathToGenericUtf8(parent);
        if (Lookup(target, path).entry.kind != "directory") {
          ConflictAt(path, Lookup(local, path), Lookup(remote, path));
          conflict_found = true;
        }
        parent = parent.parent_path();
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      status = std::move(view);
    }
    state.Save();
    Require(!conflict_found, VXCORE_ERR_SYNC_CONFLICT);
    ValidateCohort(target, false);
    return target;
  }
  void AddUpload(const std::string &canonical, const std::string &snapshot, const std::string &hash,
                 uint64_t offset, uint64_t size) {
    auto &uploads = state.pending->uploads;
    const auto found = uploads.find(canonical);
    if (found != uploads.end()) {
      Require(found->second.sha256 == hash && found->second.size == size);
      return;
    }
    Upload upload{canonical, StagingPath(state.pending->operation_id), snapshot, hash, offset, size,
                  "prepared"};
    uploads.emplace(canonical, std::move(upload));
  }
  void Plan(const Remote &remote, Versions target) {
    BeginPending();
    auto &p = *state.pending;
    const bool reuse = p.intended_head &&
                       ((remote.head && p.base_head && SameHead(*remote.head, *p.base_head)) ||
                        (!remote.head && !p.base_head)) &&
                       SameTree(Entries(target), Entries(p.target));
    p.local = local;
    p.target = std::move(target);
    p.base_head = remote.head;
    p.base_token = remote.token;
    p.phase = "prepared";
    p.apply.clear();
    if (!SameTree(Entries(p.target), remote.commit.entries)) {
      if (!reuse) {
        Commit commit;
        commit.identity.notebook_id = state.notebook_id;
        commit.identity.repository_id = remote.head ? remote.head->repository_id : NewId();
        commit.identity.generation = remote.head ? remote.head->generation + 1 : 1;
        Require(commit.identity.generation > 0);
        commit.operation_id = p.operation_id;
        commit.parent = remote.head ? remote.head->commit_hash : "";
        commit.entries = Entries(p.target);
        const auto bytes = EncodeCommit(commit);
        commit.identity.commit_hash = HashBytes(bytes);
        p.intended_head = commit.identity;
        const auto commit_snapshot = state.SnapshotBytes(bytes);
        AddUpload(CommitPath(commit.identity.commit_hash), commit_snapshot,
                  commit.identity.commit_hash, 0, bytes.size());
        const auto head_bytes = EncodeHead(commit.identity).dump();
        p.head_snapshot = state.SnapshotBytes(head_bytes);
        p.head_staging = StagingPath(p.operation_id);
      }
      std::set<std::string> committed_chunks;
      for (const auto &item : remote.commit.entries)
        for (const auto &chunk : item.second.chunks) committed_chunks.insert(chunk.sha256);
      for (const auto &item : p.target) {
        const auto &v = item.second;
        if (v.entry.kind != "file") continue;
        Require(!v.snapshot.empty());
        const auto old = Lookup(remote.commit.entries, item.first);
        if (Same(old, v.entry)) continue;
        uint64_t offset = 0;
        for (const auto &chunk : v.entry.chunks) {
          if (!committed_chunks.count(chunk.sha256))
            AddUpload(ObjectPath(chunk.sha256), v.snapshot, chunk.sha256, offset, chunk.size);
          offset += chunk.size;
        }
      }
    } else {
      Require(remote.head.has_value());
      p.intended_head.reset();
      p.head_snapshot.clear();
      p.head_staging.clear();
    }
    for (const auto &item : p.target) {
      if (webdav::IsExcluded(item.first, config)) continue;
      const auto l = Lookup(local, item.first);
      if (!Same(l.entry, item.second.entry))
        p.apply.emplace(item.first, ApplyOperation{l, item.second, "", "", "prepared"});
    }
    state.SavePending();
  }
  bool VerifyCanonical(JianguoyunTransport &transport, Upload &upload, bool allow_absent) {
    const auto verify = state.AllocateSnapshot();
    const auto result =
        transport.Download(upload.canonical, state.directory, verify, upload.sha256, upload.size);
    if (result == VXCORE_ERR_NOT_FOUND && allow_absent) return false;
    Net(transport, result);
    upload.stage = "publishedVerified";
    state.SavePending();
    return true;
  }
  void PublishUpload(JianguoyunTransport &transport, Upload &upload) {
    if (upload.stage == "publishedVerified") return;
    CheckCancelled(token);
    const bool object = upload.canonical == ObjectPath(upload.sha256);
    if (object)
      Net(transport,
          transport.EnsureCollection(".vnote-sync/objects/" + upload.sha256.substr(0, 2)));
    Net(transport, transport.EnsureCollection(".vnote-sync/staging/" +
                                              SplitPathComponents(upload.staging)[2]));
    if (upload.stage == "moveStarted" && VerifyCanonical(transport, upload, true)) return;
    const auto verify = state.AllocateSnapshot();
    const auto staged =
        transport.Download(upload.staging, state.directory, verify, upload.sha256, upload.size);
    if (staged == VXCORE_ERR_NOT_FOUND) {
      // An interrupted PUT might have left an unknown partial staging representation.
      // Reusing that name is forbidden; a fresh random intent is persisted first.
      if (upload.stage != "prepared") upload.staging = StagingPath(state.pending->operation_id);
      upload.stage = "putStarted";
      state.SavePending();
      const auto result =
          object ? transport.UploadRange(upload.staging, state.directory, upload.snapshot,
                                         upload.offset, upload.size)
                 : transport.PutBytes(upload.staging,
                                      ReadBytes(state.Resolve(upload.snapshot), kManifestBytes));
      Net(transport, result);
      upload.stage = "stagedUploaded";
      state.SavePending();
      Net(transport,
          transport.Download(upload.staging, state.directory, verify, upload.sha256, upload.size));
    } else if (staged != VXCORE_OK) {
      if (staged == VXCORE_ERR_INVALID_STATE &&
          (upload.stage == "putStarted" || upload.stage == "stagedUploaded")) {
        // An interrupted staging body is not overwritten, even though the name was ours.
        upload.staging = StagingPath(state.pending->operation_id);
        upload.stage = "prepared";
        state.SavePending();
        PublishUpload(transport, upload);
        return;
      }
      Net(transport, staged);
    }
    upload.stage = "moveStarted";
    state.SavePending();
    const auto moved = transport.MoveCreate(upload.staging, upload.canonical);
    if (moved != VXCORE_OK && moved != VXCORE_ERR_SYNC_CONFLICT &&
        moved != VXCORE_ERR_SYNC_NETWORK && moved != VXCORE_ERR_CANCELLED)
      Net(transport, moved);
    // MOVE status, including 201, never establishes immutable publication.
    if (!VerifyCanonical(transport, upload, true)) {
      Net(transport, moved == VXCORE_OK ? VXCORE_ERR_INVALID_STATE : moved);
      throw Failure{VXCORE_ERR_INVALID_STATE};
    }
  }
  void ResolvePriorPublication(JianguoyunTransport &transport, const Remote &remote) {
    if (!state.pending || !state.pending->intended_head) return;
    auto &p = *state.pending;
    const auto intended = *p.intended_head;
    if (!remote.head) {
      Require(!p.base_head && p.fresh_bootstrap);
      return;
    }
    if (remote.head->repository_id != intended.repository_id) {
      Require(state.repository_id.empty() && p.fresh_bootstrap && !p.base_head);
      // A fresh same-notebook loser adopts the winner; no losing repository is bound.
      return;
    }
    bool included = false;
    if (remote.head->generation >= intended.generation)
      included = Descends(transport, remote, intended);
    if (included) {
      // Publication is proven, but only locally equal paths become comparison facts.
      // In particular the descendant token never acknowledges the older intended tree.
      for (const auto &item : p.target) {
        const auto actual = Lookup(local, item.first);
        if (Same(actual.entry, item.second.entry)) state.entries[item.first] = item.second.entry;
      }
      state.repository_id = remote.head->repository_id;
      state.head = remote.head;
      p.phase = "published";
      state.Save();
      state.SavePending();
    } else if (p.base_head) {
      Require(remote.head->generation >= p.base_head->generation &&
              Descends(transport, remote, *p.base_head));
    }
  }
  bool PublishHead(JianguoyunTransport &transport, Remote &current) {
    auto &p = *state.pending;
    if (!p.intended_head) {
      p.phase = "published";
      state.SavePending();
      return true;
    }
    const auto intended = *p.intended_head;
    const auto bytes = ReadBytes(state.Resolve(p.head_snapshot), 4096);
    Require(bytes == EncodeHead(intended).dump());
    p.phase = "publishing";
    state.SavePending();
    VxCoreError result;
    if (p.base_head) {
      // No retry or force overwrite lives here. A rejected token means a new analysis.
      result = transport.PutBytes(kHeadPath, bytes, p.base_token);
      if (result == VXCORE_ERR_SYNC_CONFLICT) Net(transport, result);
    } else {
      Net(transport, transport.EnsureCollection(".vnote-sync/staging/" + p.operation_id));
      // An earlier interrupted staged head is abandoned, never overwritten.
      p.head_staging = StagingPath(p.operation_id);
      state.SavePending();
      std::string body, tag;
      Net(transport, transport.PutBytes(p.head_staging, bytes));
      Net(transport, transport.Get(p.head_staging, body, tag, 4096));
      Require(body == bytes);
      result = transport.MoveCreate(p.head_staging, kHeadPath);
    }
    if (result != VXCORE_OK && result != VXCORE_ERR_SYNC_CONFLICT &&
        result != VXCORE_ERR_SYNC_NETWORK && result != VXCORE_ERR_CANCELLED)
      Net(transport, result);
    const auto observed = ReadRemote(transport, false);
    Require(observed.head.has_value());
    if (observed.bytes == bytes && SameHead(*observed.head, intended)) {
      p.phase = "published";
      state.SavePending();
      current = observed;
      return true;
    }
    if (observed.head->repository_id != intended.repository_id) {
      Require(state.repository_id.empty() && p.fresh_bootstrap && !p.base_head);
      current = observed;
      return false;
    }
    if (observed.head->generation >= intended.generation &&
        Descends(transport, observed, intended)) {
      ResolvePriorPublication(transport, observed);
      current = observed;
      return false;
    }
    // A sibling or unchanged old head is retained for re-analysis on the next explicit
    // attempt. Never replace it unconditionally, even when the previous ack was lost.
    throw Failure{VXCORE_ERR_SYNC_CONFLICT};
  }
  void Exchange() {
    Require(initialized.load() && prepared, VXCORE_ERR_INVALID_STATE);
    exchanged = false;
    Progress(SyncState::kFetching, "Reading managed Jianguoyun head", 0);
    std::string username;
    auto transport = Session(username);
    Require(transport->CanonicalRoot() == state.remote_url);
    Require(username == state.username_hash || (!state.pending && state.conflicts.empty()),
            VXCORE_ERR_SYNC_IN_PROGRESS);
    if (username != state.username_hash) state.RotateUsername(username);
    JianguoyunResource root;
    Net(*transport, transport->Stat("", root));
    Require(root.collection);
    auto current = ReadRemote(*transport, true);
    ValidateHistory(*transport, current);
    ResolvePriorPublication(*transport, current);
    BeginPending();
    // At most one immediate re-analysis handles bootstrap loss or verified descendants.
    // Ordinary CAS contention returns to the consumer's existing scheduling policy.
    for (int attempt = 0; attempt < 2; ++attempt) {
      auto remote = Materialize(*transport, current.commit.entries);
      auto target = Analyze(remote);
      Plan(current, std::move(target));
      EnsureStore(*transport);
      Progress(SyncState::kPushing, "Publishing immutable Jianguoyun revisions", 0);
      // Objects precede the complete commit. Previously verified canonical objects are
      // protocol-immutable and do not need prefix revalidation on every quota retry.
      std::set<std::string> required_objects;
      for (const auto &item : state.pending->target)
        for (const auto &chunk : item.second.entry.chunks)
          required_objects.insert(ObjectPath(chunk.sha256));
      for (const auto &path : required_objects) {
        auto found = state.pending->uploads.find(path);
        if (found != state.pending->uploads.end()) PublishUpload(*transport, found->second);
      }
      if (state.pending->intended_head) {
        auto &commit =
            state.pending->uploads.at(CommitPath(state.pending->intended_head->commit_hash));
        PublishUpload(*transport, commit);
      }
      if (PublishHead(*transport, current)) {
        Require(current.head.has_value());
        // Save the exact current head. Baselines advance only in network-free Apply.
        state.repository_id = current.head->repository_id;
        state.head = current.head;
        state.Save();
        state.SavePending();
        exchanged = true;
        return;
      }
    }
    throw Failure{VXCORE_ERR_SYNC_CONFLICT};
  }
  void Preflight(const std::vector<std::string> &protected_paths, Versions &actual) {
    auto &p = *state.pending;
    for (const auto &path : protected_paths) Check(WebDavTransport::ValidateRelativePath(path));
    actual = Scan(false, false);
    bool conflict = false;
    for (auto &item : p.apply) {
      const auto &path = item.first;
      auto &op = item.second;
      const auto current = Lookup(actual, path);
      const bool expected = RawSame(current, op.expected);
      const bool intended = Same(current.entry, op.desired.entry);
      if ((!expected && !intended) || (!intended && Protected(path, protected_paths))) {
        auto retained = ReadLocal(path, true);
        ConflictAt(path, retained, op.desired);
        conflict = true;
      }
      if (!intended && current.entry.kind == "directory" && op.desired.entry.kind != "directory") {
        for (const auto &child : actual) {
          if (child.first.compare(0, path.size() + 1, path + "/") != 0) continue;
          const auto deletion = p.apply.find(child.first);
          if (deletion == p.apply.end() || Present(deletion->second.desired.entry)) conflict = true;
        }
        fs::path directory;
        Check(WebDavTransport::ResolveLocalPath(state.root, path, directory));
        // Excluded children were not in the logical scan; they still prohibit removal.
        for (const auto &child : fs::recursive_directory_iterator(directory)) {
          Require(CheckReparsePoint(PathToUtf8(child.path())) == ReparseState::kNo,
                  VXCORE_ERR_UNSUPPORTED);
          const auto relative = PathToGenericUtf8(child.path().lexically_relative(state.root));
          if (!actual.count(relative)) conflict = true;
        }
      }
    }
    if (conflict) {
      state.Save();
      throw Failure{VXCORE_ERR_SYNC_CONFLICT};
    }
    Versions cohort = actual;
    for (const auto &item : p.apply) cohort[item.first] = item.second.desired;
    ValidateCohort(cohort, true);
    // Resolve EVERY incoming native path before the first install, including aliases
    // between two as-yet nonexistent destinations (ValidateTree above).
    for (const auto &item : p.apply) {
      fs::path path;
      std::string prefix;
      bool future_parent = false;
      const auto parts = SplitPathComponents(item.first);
      for (size_t index = 0; index < parts.size(); ++index) {
        if (!prefix.empty()) prefix += "/";
        prefix += parts[index];
        Check(WebDavTransport::ResolveLocalPath(state.root, prefix, path));
        const auto replacement = p.apply.find(prefix);
        if (index + 1 < parts.size() && replacement != p.apply.end() &&
            replacement->second.desired.entry.kind == "directory" &&
            Lookup(actual, prefix).entry.kind == "file") {
          future_parent = true;
          break;
        }
      }
      if (!future_parent) Check(WebDavTransport::ResolveLocalPath(state.root, item.first, path));
    }
  }
  void Apply(const std::vector<std::string> &protected_paths, std::vector<std::string> &changed) {
    Require(initialized.load() && state.pending &&
                ((prepared && exchanged) || state.pending->phase == "published" ||
                 state.pending->phase == "applying"),
            VXCORE_ERR_INVALID_STATE);
    CheckCancelled(token);
    Progress(SyncState::kMerging, "Installing managed Jianguoyun revisions", 0);
    Versions actual;
    Preflight(protected_paths, actual);
    auto &p = *state.pending;
    if (!state.head) {
      Require(p.intended_head || p.base_head);
      state.head = p.intended_head ? p.intended_head : p.base_head;
      state.repository_id = state.head->repository_id;
      state.Save();
    }
    p.phase = "applying";
    state.SavePending();
    std::vector<std::string> order;
    for (const auto &item : p.apply) order.push_back(item.first);
    auto rank = [&](const std::string &path) {
      const auto &op = p.apply.at(path);
      if (!Present(op.desired.entry)) return 0;
      if (op.desired.entry.kind == "directory") return 1;
      if (path == kConfigPath) return 4;
      return webdav::IsMetadata(path) ? 3 : 2;
    };
    std::sort(order.begin(), order.end(), [&](const std::string &a, const std::string &b) {
      const auto ar = rank(a), br = rank(b);
      if (ar != br) return ar < br;
      if (ar == 0) return a.size() == b.size() ? a < b : a.size() > b.size();
      if (ar == 1) return a.size() == b.size() ? a < b : a.size() < b.size();
      return a < b;
    });
    for (const auto &path : order) {
      CheckCancelled(token);
      auto &op = p.apply.at(path);
      auto current = ReadLocal(path, false);
      const bool intended = Same(current.entry, op.desired.entry);
      Require(RawSame(current, op.expected) || intended, VXCORE_ERR_SYNC_CONFLICT);
      fs::path destination;
      Check(WebDavTransport::ResolveLocalPath(state.root, path, destination));
      bool installed = false;
      if (!intended) {
        if (current.entry.kind == "file") {
          op.backup = state.SnapshotFile(destination);
          op.backup_sha256 = current.raw_sha256;
          state.VerifySnapshot(op.backup, op.backup_sha256);
        }
        op.stage = "installing";
        state.SavePending();
        if (current.entry.kind == "directory" && op.desired.entry.kind != "directory")
          Require(fs::is_empty(destination) && fs::remove(destination), VXCORE_ERR_SYNC_CONFLICT);
        else if (current.entry.kind == "file" && op.desired.entry.kind != "file")
          Require(fs::remove(destination), VXCORE_ERR_IO);
        if (op.desired.entry.kind == "file") {
          state.VerifySnapshot(op.desired.snapshot, op.desired.entry.sha256, op.desired.entry.size);
          if (path == kConfigPath && fs::exists(destination)) {
            const auto routing = webdav::NotebookJson(destination, state.notebook_id);
            Check(WriteFileAtomic(
                destination,
                webdav::RestoreRouting(ReadBytes(state.Resolve(op.desired.snapshot)), routing)));
          } else
            webdav::CopyAtomic(state.Resolve(op.desired.snapshot), destination, token);
        } else if (op.desired.entry.kind == "directory") {
          Require(fs::create_directory(destination), VXCORE_ERR_IO);
        }
        installed = true;
      }
      if (installed || op.stage == "installing" || op.stage == "localConfirmed")
        changed.push_back(path);
      op.stage = "localConfirmed";
      state.SavePending();
      const auto observed = ReadLocal(path, false);
      Require(Same(observed.entry, op.desired.entry), VXCORE_ERR_SYNC_CONFLICT);
      state.entries[path] = op.desired.entry;
      state.conflicts.erase(path);
      state.Save();
      op.stage = "baselineConfirmed";
      state.SavePending();
    }
    // Equal outgoing and unchanged paths are acknowledged only if the raw staged bytes
    // remain equal. A newly edited path keeps its previous baseline and dirty state.
    for (const auto &item : p.target) {
      if (webdav::IsExcluded(item.first, config)) continue;
      const auto observed = ReadLocal(item.first, false);
      const auto staged = Lookup(p.local, item.first);
      if (Same(observed.entry, item.second.entry) &&
          (p.apply.count(item.first) || RawSame(observed, staged))) {
        state.entries[item.first] = item.second.entry;
        state.conflicts.erase(item.first);
      }
    }
    state.Save();
    state.ClearPending();
    state.CollectSnapshots();
    local.clear();
    prepared = exchanged = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      status.clear();
    }
    Progress(SyncState::kIdle, "Jianguoyun synchronization complete", 100);
  }
  void Clone(const std::string &target, const SyncConfig &cfg) {
    initialized.store(false);
    prepared = exchanged = false;
    config = cfg;
    Require(config.backend == "jianguoyun", VXCORE_ERR_INVALID_PARAM);
    const auto root = fs::absolute(PathFromUtf8(target));
    Require(fs::is_directory(root) && fs::is_empty(root) &&
                CheckReparsePoint(PathToUtf8(root)) == ReparseState::kNo,
            VXCORE_ERR_INVALID_PARAM);
    state = State{};
    state.cancellation = token;
    local.clear();
    std::string username;
    auto transport = Session(username);
    const auto remote = ReadRemote(*transport, false);
    Require(remote.head.has_value());
    state.Open(root, remote.head->notebook_id, transport->CanonicalRoot(), username);
    BeginPending();
    auto downloaded = Materialize(*transport, remote.commit.entries);
    ValidateCohort(downloaded, false);
    const auto verified = ReadRemote(*transport, false);
    Require(verified.head && SameHead(*remote.head, *verified.head), VXCORE_ERR_SYNC_CONFLICT);
    state.repository_id = remote.head->repository_id;
    state.head = remote.head;
    auto &p = *state.pending;
    p.base_head = remote.head;
    p.base_token = remote.token;
    p.target = std::move(downloaded);
    p.phase = "published";
    for (const auto &item : p.target)
      if (Present(item.second.entry))
        p.apply.emplace(item.first, ApplyOperation{{}, item.second, "", "", "prepared"});
    state.Save();
    state.SavePending();
    initialized.store(true);
    prepared = exchanged = true;
    // Clone owns an empty root, apart from its private recovery directory. Treat only
    // that namespace as preexisting, and use the same atomic per-path installer.
    std::vector<std::string> changed;
    Apply({}, changed);
  }
};

JianguoyunSyncBackend::JianguoyunSyncBackend(const SyncConfig &config,
                                             std::shared_ptr<ICredentialProvider> provider)
    : impl_(std::make_unique<Impl>(config, std::move(provider))) {}
JianguoyunSyncBackend::~JianguoyunSyncBackend() = default;
std::string JianguoyunSyncBackend::GetName() const { return "jianguoyun"; }
SyncCapabilities JianguoyunSyncBackend::GetCapabilities() const {
  return static_cast<uint32_t>(SyncCapability::ConflictDetection) |
         static_cast<uint32_t>(SyncCapability::ConflictResolution) |
         static_cast<uint32_t>(SyncCapability::IncrementalSync) |
         static_cast<uint32_t>(SyncCapability::AuthRequired) |
         static_cast<uint32_t>(SyncCapability::Cancellation) |
         static_cast<uint32_t>(SyncCapability::ProgressReporting) |
         static_cast<uint32_t>(SyncCapability::Cloneable) |
         static_cast<uint32_t>(SyncCapability::DeferredLocalApply);
}
bool JianguoyunSyncBackend::IsInitialized() const { return impl_->initialized.load(); }
VxCoreError JianguoyunSyncBackend::Initialize(const std::string &root, const SyncConfig &config) {
  return impl_->Run([&] { impl_->Initialize(root, config); });
}
VxCoreError JianguoyunSyncBackend::Clone(const std::string &target, const SyncConfig &config) {
  return impl_->Run([&] {
    impl_->initialized.store(false);
    impl_->Clone(target, config);
  });
}
void JianguoyunSyncBackend::ReplaceCredsProvider(std::shared_ptr<ICredentialProvider> provider) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->provider = std::move(provider);
}
std::shared_ptr<ICredentialProvider> JianguoyunSyncBackend::GetCredsProviderSnapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->provider;
}
void JianguoyunSyncBackend::SetCancellation(SyncCancellationPtr token) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->cancellation = std::move(token);
}
VxCoreError JianguoyunSyncBackend::Sync(SyncProgressCallback callback, void *userdata) {
  return impl_->Run([&] {
    impl_->callback = std::move(callback);
    impl_->userdata = userdata;
    struct Clear {
      Impl &impl;
      ~Clear() {
        impl.callback = {};
        impl.userdata = nullptr;
      }
    } clear{*impl_};
    impl_->Stage();
    impl_->Exchange();
    std::vector<std::string> changed;
    impl_->Apply({}, changed);
  });
}
VxCoreError JianguoyunSyncBackend::StageAndCommit(bool *out_did_commit) {
  if (out_did_commit) *out_did_commit = false;
  return impl_->Run([&] { impl_->Stage(); });
}
VxCoreError JianguoyunSyncBackend::FetchRebasePush() {
  return impl_->Run([&] { impl_->Exchange(); });
}
VxCoreError JianguoyunSyncBackend::ApplySync(const std::vector<std::string> &protected_paths,
                                             std::vector<std::string> &out_changed_paths) {
  out_changed_paths.clear();
  return impl_->Run([&] { impl_->Apply(protected_paths, out_changed_paths); });
}
VxCoreError JianguoyunSyncBackend::GetStatus(std::vector<SyncFileInfo> &out_files) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  out_files = impl_->status;
  return VXCORE_OK;
}
VxCoreError JianguoyunSyncBackend::GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  out_conflicts = impl_->public_conflicts;
  return VXCORE_OK;
}
VxCoreError JianguoyunSyncBackend::ResolveConflict(const std::string &path,
                                                   SyncConflictResolution resolution) {
  return impl_->Run([&] {
    Require(impl_->initialized.load(), VXCORE_ERR_INVALID_STATE);
    ValidatePath(path);
    auto found = impl_->state.conflicts.find(path);
    Require(found != impl_->state.conflicts.end(), VXCORE_ERR_NOT_FOUND);
    Require(resolution == SyncConflictResolution::kKeepLocal ||
                resolution == SyncConflictResolution::kKeepRemote ||
                resolution == SyncConflictResolution::kKeepBoth,
            VXCORE_ERR_INVALID_PARAM);
    Require(resolution != SyncConflictResolution::kKeepBoth || found->second.can_keep_both,
            VXCORE_ERR_UNSUPPORTED);
    const auto previous = found->second.resolution;
    found->second.resolution = resolution == SyncConflictResolution::kKeepLocal    ? "keep_local"
                               : resolution == SyncConflictResolution::kKeepRemote ? "keep_remote"
                                                                                   : "keep_both";
    try {
      impl_->state.Save();
    } catch (...) {
      found->second.resolution = previous;
      throw;
    }
  });
}
std::string JianguoyunSyncBackend::GetLastError() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->last_error;
}

}  // namespace vxcore
