#include "webdav_sync_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <utility>

#include "core/notebook.h"
#include "sync/credential_provider.h"
#include "sync/sync_json_keys.h"
#include "utils/file_utils.h"
#include "vxcore/notebook_json_keys.h"
#include "webdav_state.h"
#include "webdav_transport.h"

namespace vxcore {
namespace fs = std::filesystem;
using namespace webdav;
namespace {

const char *Diagnostic(VxCoreError error) {
  switch (error) {
    case VXCORE_OK:
      return "";
    case VXCORE_ERR_CANCELLED:
      return "WebDAV synchronization was cancelled. Recovery data has been retained.";
    case VXCORE_ERR_SYNC_AUTH_FAILED:
      return "WebDAV authentication failed. Check the username and password or app password.";
    case VXCORE_ERR_PERMISSION_DENIED:
      return "The WebDAV account does not have permission for this operation.";
    case VXCORE_ERR_NOT_FOUND:
      return "The dedicated WebDAV notebook collection was not found.";
    case VXCORE_ERR_UNSUPPORTED:
      return "WebDAV requires safe paths, strong ETags and conditional file operations. Check the "
             "server and notebook configuration.";
    case VXCORE_ERR_SYNC_NETWORK:
      return "The WebDAV request failed. Check the connection and trusted TLS certificate, then "
             "retry.";
    case VXCORE_ERR_SYNC_CONFLICT:
      return "WebDAV found conflicting changes. Both revisions have been retained for resolution.";
    case VXCORE_ERR_SYNC_IN_PROGRESS:
      return "The notebook changed or is busy. Retry synchronization when current work finishes.";
    case VXCORE_ERR_IO:
      return "WebDAV could not safely read or publish local recovery data. Check storage and "
             "permissions.";
    case VXCORE_ERR_ENCRYPTION_FORMAT:
      return "The synchronized encryption envelope is invalid. The current revision was preserved.";
    case VXCORE_ERR_INVALID_PARAM:
      return "The WebDAV settings or path are invalid.";
    default:
      return "WebDAV notebook identity, metadata or recovery state is invalid. Existing content "
             "was preserved.";
  }
}

bool IsLocalAction(const Operation &op) {
  return op.action == "upsertLocal" || op.action == "deleteLocal" || op.action == "mkdirLocal" ||
         op.action == "removeLocalCollection";
}
bool IsRemoteAction(const Operation &op) { return !IsLocalAction(op); }

Version BaselineVersion(const Version &version) {
  // A baseline is a durable comparison fact, not an owner of operation snapshots.
  // Keeping a snapshot name here would reuse a removed payload on the next round.
  Version baseline;
  baseline.kind = version.kind;
  baseline.sha256 = version.sha256;
  baseline.etag = version.etag;
  return baseline;
}

int Rank(const Operation &op) {
  if (op.action == "mkdirLocal" || op.action == "mkdirRemote") return 0;
  if (op.action == "deleteLocal" || op.action == "deleteRemote" ||
      op.action == "removeLocalCollection")
    return 4;
  if (op.path == kConfigPath) return 3;
  return IsMetadata(op.path) ? 2 : 1;
}
void SortOperations(std::vector<Operation> &operations) {
  std::stable_sort(operations.begin(), operations.end(),
                   [](const Operation &a, const Operation &b) {
                     const auto ra = Rank(a), rb = Rank(b);
                     if (ra != rb) return ra < rb;
                     if (ra == 4) return a.path.size() > b.path.size();
                     if (ra == 0) return a.path.size() < b.path.size();
                     return a.path < b.path;
                   });
}

bool Protected(const std::string &path, const std::vector<std::string> &protected_paths) {
  for (const auto &p : protected_paths)
    if (path == p || path.compare(0, p.size() + 1, p + "/") == 0 ||
        p.compare(0, path.size() + 1, path + "/") == 0)
      return true;
  return false;
}

std::string UsernameHash(const SyncCredentials &creds) {
  if (!creds.extra.is_object()) return HashBytes("");
  auto it = creds.extra.find(kJsonKeyUsername);
  Require(it == creds.extra.end() || it->is_string(), VXCORE_ERR_INVALID_PARAM);
  return HashBytes(it == creds.extra.end() ? "" : it->get<std::string>());
}

bool HasCredentials(const SyncCredentials &creds) {
  return creds.extra.is_object() && creds.extra.contains(kJsonKeyUsername) &&
         creds.extra.at(kJsonKeyUsername).is_string() &&
         !creds.extra.at(kJsonKeyUsername).get_ref<const std::string &>().empty() &&
         creds.extra.contains(kJsonKeyPassword) && creds.extra.at(kJsonKeyPassword).is_string() &&
         !creds.extra.at(kJsonKeyPassword).get_ref<const std::string &>().empty();
}

}  // namespace

struct WebDavSyncBackend::Impl {
  explicit Impl(const SyncConfig &cfg, std::shared_ptr<ICredentialProvider> creds)
      : config(cfg), provider(std::move(creds)) {}

  SyncConfig config;
  std::shared_ptr<ICredentialProvider> provider;
  std::shared_ptr<ICredentialProvider> active_provider;
  SyncCancellationPtr cancellation;
  mutable std::mutex mutex;
  std::atomic<bool> busy{false};
  std::atomic<bool> initialized{false};
  std::vector<SyncFileInfo> status;
  std::vector<SyncConflictInfo> public_conflicts;
  std::string last_error;
  State state;
  Tree local;
  Tree remote;
  Tree common;
  bool prepared = false;
  bool exchanged = false;
  bool bootstrap = false;
  std::string verified_username;
  SyncProgressCallback callback;
  void *userdata = nullptr;
  SyncCancellationPtr token;

  template <typename Function>
  VxCoreError Run(Function function) {
    if (busy.exchange(true)) return VXCORE_ERR_SYNC_IN_PROGRESS;
    struct Release {
      std::atomic<bool> &flag;
      ~Release() { flag.store(false); }
    } release{busy};
    {
      std::lock_guard<std::mutex> lock(mutex);
      last_error.clear();
      token = cancellation;
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
    } catch (const nlohmann::json::exception &) {
      result = VXCORE_ERR_INVALID_STATE;
    } catch (const std::exception &) {
      result = VXCORE_ERR_INVALID_STATE;
    } catch (...) {
      result = VXCORE_ERR_UNKNOWN;
    }
    PublishConflicts();
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (last_error.empty()) last_error = Diagnostic(result);
    }
    token.reset();
    state.cancellation.reset();
    active_provider.reset();
    return result;
  }

  void Progress(SyncState phase, const char *message, float percent) {
    if (callback) callback(SyncProgress{message, percent, phase}, userdata);
  }
  void PublishConflicts() {
    std::vector<SyncConflictInfo> snapshot;
    for (const auto &entry : state.conflicts) {
      const auto &c = entry.second;
      snapshot.push_back({entry.first, c.local.modified_utc, c.remote.modified_utc,
                          c.local.binary || c.remote.binary, c.can_keep_both});
    }
    std::lock_guard<std::mutex> lock(mutex);
    public_conflicts.swap(snapshot);
  }
  void Collision() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      last_error = "A file and folder use the same path. Rename one of them, then sync again.";
    }
    throw Failure{VXCORE_ERR_UNSUPPORTED};
  }

  std::unique_ptr<WebDavTransport> Session(std::string &username, bool writable) {
    std::shared_ptr<ICredentialProvider> snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex);
      snapshot = provider;
    }
    Require(snapshot != nullptr, VXCORE_ERR_INVALID_PARAM);
    active_provider = snapshot;
    SyncCredentials credentials;
    snapshot->GetCredentials(config.remote_url, "", &credentials);
    Require(!writable || HasCredentials(credentials), VXCORE_ERR_SYNC_AUTH_FAILED);
    username = UsernameHash(credentials);
    auto transport = std::make_unique<WebDavTransport>(config.remote_url, credentials, token);
    Check(transport->Initialize());
    transport->SetProgressCallback([this](uint64_t bytes, uint64_t total) {
      Progress(SyncState::kFetching, "Transferring WebDAV bytes",
               total ? static_cast<float>(100.0 * static_cast<double>(bytes) / total) : 0.0f);
    });
    return transport;
  }

  Version Download(WebDavTransport &transport, const WebDavResource &resource, State &storage) {
    Require(resource.kind == WebDavResourceKind::kFile);
    Version version;
    version.kind = "file";
    version.etag = resource.etag;
    version.modified_utc = resource.modified_utc;
    version.snapshot = storage.AllocateSnapshot();
    Check(transport.Download(resource.path, resource.etag, storage.directory, version.snapshot));
    version.sha256 = transport.LastResponse().sha256;
    version.raw_sha256 = version.sha256;
    storage.VerifySnapshot(version.snapshot, version.sha256);
    if (!IsScratch(resource.path)) {
      ValidateMetadata(resource.path, storage.Resolve(version.snapshot), storage.notebook_id);
      if (resource.path == kConfigPath) {
        const auto projected =
            ProjectConfig(NotebookJson(storage.Resolve(version.snapshot), storage.notebook_id));
        const auto original = version.snapshot;
        version.sha256 = HashBytes(projected);
        version.snapshot = storage.SnapshotBytes(projected);
        storage.RemoveSnapshot(original);
      }
    }
    std::ifstream input(storage.Resolve(version.snapshot), std::ios::binary);
    Require(input.good(), VXCORE_ERR_IO);
    char sample[8192];
    input.read(sample, sizeof(sample));
    version.binary = IsEncrypted(resource.path) ||
                     std::find(sample, sample + input.gcount(), '\0') != sample + input.gcount();
    return version;
  }

  Version Inspect(WebDavTransport &transport, const std::string &path, bool body = true,
                  State *storage = nullptr) {
    WebDavResource resource;
    const auto result = transport.Stat(path, resource);
    if (result == VXCORE_ERR_NOT_FOUND) return {};
    Check(result);
    if (resource.kind == WebDavResourceKind::kCollection) {
      Version v;
      v.kind = "collection";
      return v;
    }
    if (body) return Download(transport, resource, storage ? *storage : state);
    Version v;
    v.kind = "file";
    v.etag = resource.etag;
    v.modified_utc = resource.modified_utc;
    return v;
  }

  void ListRemote(WebDavTransport &transport, bool reuse_hashes) {
    std::vector<WebDavResource> resources;
    Check(transport.List(resources));
    Tree next;
    for (const auto &resource : resources) {
      CheckCancelled(token);
      if (resource.path.empty() || IsScratch(resource.path) || IsExcluded(resource.path, config))
        continue;
      fs::path checked;
      Check(WebDavTransport::ResolveLocalPath(state.root, resource.path, checked));
      Version v;
      if (resource.kind == WebDavResourceKind::kCollection)
        v.kind = "collection";
      else {
        const auto base = Lookup(state.entries, resource.path);
        if (reuse_hashes && resource.path != kConfigPath && base.kind == "file" &&
            base.etag == resource.etag) {
          v = base;
          v.modified_utc = resource.modified_utc;
        } else
          v = Download(transport, resource, state);
      }
      next.emplace(resource.path, std::move(v));
    }
    remote.swap(next);
  }

  void EnsureRemoteSnapshot(WebDavTransport &transport, const std::string &path) {
    auto it = remote.find(path);
    if (it == remote.end() || it->second.kind != "file" || !it->second.snapshot.empty()) return;
    const auto expected = it->second;
    auto actual = Inspect(transport, path);
    Require(actual.kind == "file" && actual.etag == expected.etag && Same(actual, expected),
            VXCORE_ERR_SYNC_CONFLICT);
    it->second = std::move(actual);
  }

  void VerifyIdentity(WebDavTransport &transport, bool allow_empty) {
    std::vector<WebDavResource> resources;
    Check(transport.List(resources));
    const WebDavResource *identity = nullptr;
    bool ordinary = false;
    for (const auto &r : resources) {
      if (r.path.empty() || IsScratch(r.path)) continue;
      ordinary = true;
      if (r.path == kConfigPath && r.kind == WebDavResourceKind::kFile) identity = &r;
    }
    if (!identity) {
      // A previous config-first bootstrap may have created only its required parent.
      bool own_parent_only =
          std::any_of(state.operations.begin(), state.operations.end(),
                      [](const Operation &op) {
                        return op.path == "vx_notebook" && op.action == "mkdirRemote" &&
                               op.expected_remote_kind == "absent";
                      }) &&
          std::any_of(state.operations.begin(), state.operations.end(), [](const Operation &op) {
            return op.path == kConfigPath && op.action == "upsertRemote" &&
                   op.expected_remote_kind == "absent";
          });
      for (const auto &r : resources)
        if (!r.path.empty() && !IsScratch(r.path) &&
            !(r.path == "vx_notebook" && r.kind == WebDavResourceKind::kCollection))
          own_parent_only = false;
      Require(allow_empty && state.entries.empty() && (!ordinary || own_parent_only));
      bootstrap = true;
      return;
    }
    auto downloaded = Download(transport, *identity, state);
    state.RemoveSnapshot(downloaded.snapshot);
    bootstrap = false;
  }

  void ProbeCleanup(WebDavTransport &transport, State &probe) {
    for (const auto &op : probe.operations) {
      Require(IsScratch(op.path) && op.path.find('/') == std::string::npos);
      auto actual = Inspect(transport, op.path, true, &probe);
      if (actual.kind == "absent") continue;
      Require(actual.kind == "file", VXCORE_ERR_UNSUPPORTED);
      bool owned = false;
      for (const auto &known : probe.operations)
        if (actual.sha256 == known.new_sha256 ||
            (!known.old_sha256.empty() && actual.sha256 == known.old_sha256))
          owned = true;
      Require(owned, VXCORE_ERR_INVALID_STATE);
      Check(transport.RemoveFile(op.path, actual.etag));
      Require(Inspect(transport, op.path, false, &probe).kind == "absent", VXCORE_ERR_UNSUPPORTED);
      probe.RemoveSnapshot(actual.snapshot);
    }
    const auto completed = probe.operations;
    probe.ClearPending();
    probe.RemoveUnreferencedSnapshots(completed);
    probe.CleanupTransient();
  }

  void Probe(WebDavTransport &transport) {
    State probe;
    probe.cancellation = token;
    probe.Open(state.root, state.notebook_id, state.remote_url, state.username_hash,
               std::string(kPrivatePath) + "/probe");
    if (!probe.operations.empty()) ProbeCleanup(transport, probe);
    probe.RotateUsername(state.username_hash);
    probe.Save();
    const auto a = std::string(kScratchPrefix) + NewId();
    const auto b = std::string(kScratchPrefix) + NewId();
    const auto c = std::string(kScratchPrefix) + NewId();
    const auto bytes_a = NewId() + NewId();
    const auto bytes_b = NewId() + NewId();
    Operation first;
    first.path = a;
    first.action = "upsertRemote";
    first.new_sha256 = HashBytes(bytes_a);
    first.source_snapshot = probe.SnapshotBytes(bytes_a);
    Operation second;
    second.path = b;
    second.action = "upsertRemote";
    second.new_sha256 = HashBytes(bytes_b);
    second.source_snapshot = probe.SnapshotBytes(bytes_b);
    Operation third = first;
    third.path = c;
    probe.operations = {first, second, third};
    probe.SavePending();
    try {
      Check(transport.Upload(a, probe.directory, first.source_snapshot, ""));
      Check(transport.Upload(b, probe.directory, second.source_snapshot, ""));
      auto av = Inspect(transport, a, true, &probe);
      auto bv = Inspect(transport, b, true, &probe);
      Require(av.sha256 == first.new_sha256 && bv.sha256 == second.new_sha256,
              VXCORE_ERR_UNSUPPORTED);
      probe.operations[0].stage = "remoteConfirmed";
      probe.operations[1].stage = "remoteConfirmed";
      probe.SavePending();
      auto reject = [&](VxCoreError result) {
        Require(result == VXCORE_ERR_SYNC_CONFLICT && transport.LastResponse().http_status == 412,
                VXCORE_ERR_UNSUPPORTED);
      };
      reject(transport.Upload(a, probe.directory, second.source_snapshot, ""));
      reject(transport.Move(a, av.etag, b, "\"vnote-invalid-" + NewId() + "\""));
      Require(Inspect(transport, a, true, &probe).sha256 == av.sha256 &&
                  Inspect(transport, b, true, &probe).sha256 == bv.sha256,
              VXCORE_ERR_UNSUPPORTED);
      reject(transport.Move(a, av.etag, b, ""));
      Require(Inspect(transport, a, true, &probe).sha256 == av.sha256 &&
                  Inspect(transport, b, true, &probe).sha256 == bv.sha256,
              VXCORE_ERR_UNSUPPORTED);
      // All possible destinations and complete payloads were journaled before either MOVE.
      probe.operations[1].previous_snapshot = second.source_snapshot;
      probe.operations[1].old_sha256 = second.new_sha256;
      probe.operations[1].source_snapshot = first.source_snapshot;
      probe.operations[1].new_sha256 = first.new_sha256;
      probe.SavePending();
      Check(transport.Move(a, av.etag, b, bv.etag));
      bv = Inspect(transport, b, true, &probe);
      Require(bv.sha256 == av.sha256 && Inspect(transport, a, false, &probe).kind == "absent",
              VXCORE_ERR_UNSUPPORTED);
      reject(transport.RemoveFile(b, "\"vnote-invalid-" + NewId() + "\""));
      Require(Inspect(transport, b, true, &probe).sha256 == av.sha256, VXCORE_ERR_UNSUPPORTED);
      Check(transport.Move(b, bv.etag, c, ""));
      auto cv = Inspect(transport, c, true, &probe);
      Require(cv.sha256 == av.sha256 && Inspect(transport, b, false, &probe).kind == "absent",
              VXCORE_ERR_UNSUPPORTED);
      ProbeCleanup(transport, probe);
    } catch (...) {
      // A cancelled or broken connection is not a reason to send more mutations. Its
      // owned probe journal remains recoverable by the next explicit Initialize.
      throw;
    }
  }

  void Initialize(const std::string &root, const SyncConfig &cfg) {
    initialized.store(false);
    prepared = false;
    exchanged = false;
    local.clear();
    remote.clear();
    common.clear();
    state = State{};
    state.cancellation = token;
    {
      std::lock_guard<std::mutex> lock(mutex);
      status.clear();
      public_conflicts.clear();
    }
    config = cfg;
    Require(config.backend == "webdav", VXCORE_ERR_INVALID_PARAM);
    const auto root_path = fs::absolute(PathFromUtf8(root));
    Require(fs::is_directory(root_path) &&
                CheckReparsePoint(PathToUtf8(root_path)) == ReparseState::kNo,
            VXCORE_ERR_UNSUPPORTED);
    fs::path config_file;
    Check(WebDavTransport::ResolveLocalPath(root_path, kConfigPath, config_file));
    const auto notebook = NotebookJson(config_file);
    std::string username;
    auto transport = Session(username, true);
    state.Open(root_path, notebook.at(kJsonKeyId).get<std::string>(), transport->CanonicalRoot(),
               username);
    VerifyIdentity(*transport, true);
    if (!state.existed) state.Save();
    if (state.operations.empty()) state.RotateUsername(username);
    // Existing recovery is bound to its original authenticated account until it completes.
    Probe(*transport);
    initialized.store(true);
  }

  void ScanLocal() {
    Tree next;
    std::vector<std::string> ids;
    fs::recursive_directory_iterator it(state.root), end;
    for (; it != end; ++it) {
      CheckCancelled(token);
      Require(it.depth() < 256 && next.size() < 250000, VXCORE_ERR_UNSUPPORTED);
      const auto relative = PathToGenericUtf8(it->path().lexically_relative(state.root));
      Check(WebDavTransport::ValidateRelativePath(relative));
      if (IsExcluded(relative, config)) {
        if (it->is_directory()) it.disable_recursion_pending();
        continue;
      }
      if (IsScratch(relative)) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          last_error =
              "A local name uses the reserved .vnote-webdav-tmp- prefix. Rename it before "
              "synchronization.";
        }
        throw Failure{VXCORE_ERR_UNSUPPORTED};
      }
      Require(CheckReparsePoint(PathToUtf8(it->path())) == ReparseState::kNo,
              VXCORE_ERR_UNSUPPORTED);
      auto version = ReadLocal(state.root, relative, state.notebook_id, token);
      if (version.kind == "file") {
        const auto base = Lookup(state.entries, relative);
        if (!Same(version, base) || state.conflicts.count(relative)) {
          if (relative == kConfigPath)
            version.snapshot =
                state.SnapshotBytes(ProjectConfig(NotebookJson(it->path(), state.notebook_id)));
          else
            version.snapshot = state.SnapshotFile(it->path(), token);
          Require(HashFile(state.Resolve(version.snapshot), token) == version.sha256,
                  VXCORE_ERR_SYNC_IN_PROGRESS);
        }
        ValidateMetadata(relative, it->path(), state.notebook_id, &ids);
      }
      Require(next.emplace(relative, std::move(version)).second);
    }
    std::set<std::string> unique;
    for (const auto &id : ids) Require(unique.insert(id).second);
    Require(next.count(kConfigPath) && next.at(kConfigPath).kind == "file");
    local.swap(next);
  }

  void Stage() {
    Require(initialized.load(), VXCORE_ERR_INVALID_STATE);
    prepared = false;
    exchanged = false;
    state.CleanupTransient();
    local.clear();
    remote.clear();
    common.clear();
    Progress(SyncState::kStaging, "Preparing local WebDAV snapshot", 0);
    ScanLocal();
    CheckCancelled(token);
    prepared = true;
  }

  void EnsureLocalSnapshot(const std::string &path) {
    auto it = local.find(path);
    if (it == local.end() || it->second.kind != "file" || !it->second.snapshot.empty()) return;
    const auto actual = ReadLocal(state.root, path, state.notebook_id, token, &state);
    Require(actual.raw_sha256 == it->second.raw_sha256 && Same(actual, it->second),
            VXCORE_ERR_SYNC_IN_PROGRESS);
    it->second = actual;
  }

  void ConflictAt(WebDavTransport *transport, const std::string &path, bool current_local = false) {
    if (current_local) {
      auto actual = ReadLocal(state.root, path, state.notebook_id, token, &state);
      if (actual.kind == "absent")
        local.erase(path);
      else
        local[path] = std::move(actual);
    }
    EnsureLocalSnapshot(path);
    if (transport) EnsureRemoteSnapshot(*transport, path);
    const auto l = Lookup(local, path), r = Lookup(remote, path);
    if (l.kind == "collection" || r.kind == "collection") Collision();
    Conflict conflict;
    conflict.local = l;
    conflict.remote = r;
    conflict.can_keep_both = !IsMetadata(path) && !IsEncrypted(path);
    Require(l.kind != "file" || !l.snapshot.empty());
    Require(r.kind != "file" || !r.snapshot.empty());
    state.conflicts[path] = std::move(conflict);
  }

  Operation Plan(const std::string &path, const std::string &action, const Version &l,
                 const Version &r, const Version &desired) {
    Operation op;
    op.path = path;
    op.action = action;
    op.kind = desired.kind == "collection" || l.kind == "collection" || r.kind == "collection"
                  ? "collection"
                  : "file";
    op.expected_local_kind = l.kind;
    op.expected_local_sha256 = l.raw_sha256;
    op.expected_remote_kind = r.kind;
    op.expected_remote_etag = r.etag;
    if (action == "upsertLocal" || action == "upsertRemote") {
      op.new_sha256 = desired.sha256;
      op.source_snapshot = desired.snapshot;
      Require(!op.source_snapshot.empty());
      if (action == "upsertRemote") op.scratch_url = std::string(kScratchPrefix) + NewId();
    }
    const auto &previous = IsLocalAction(op) ? l : r;
    if (previous.kind == "file" && !previous.snapshot.empty()) {
      op.old_sha256 = previous.sha256;
      op.previous_snapshot = previous.snapshot;
      Require(!op.previous_snapshot.empty());
    }
    return op;
  }

  std::string ConflictName(const std::string &path) {
    const auto p = PathFromUtf8(path);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const auto name = PathToUtf8(p.stem()) + ".sync-conflict-" + std::to_string(now) + "-" +
                      NewId() + PathToUtf8(p.extension());
    const auto result = PathToGenericUtf8(p.parent_path() / PathFromUtf8(name));
    Require(!local.count(result) && !remote.count(result) && !state.entries.count(result));
    return result;
  }

  void AddPlan(WebDavTransport &transport, std::vector<Operation> &plan, const std::string &path,
               bool to_remote, Version desired) {
    if (to_remote) EnsureLocalSnapshot(path);
    EnsureRemoteSnapshot(transport, path);
    const auto l = Lookup(local, path), r = Lookup(remote, path);
    if (desired.kind == "file" && desired.snapshot.empty()) {
      desired = to_remote ? l : r;
      Require(!desired.snapshot.empty());
    }
    if (desired.kind == "collection") {
      plan.push_back(Plan(path, to_remote ? "mkdirRemote" : "mkdirLocal", l, r, desired));
    } else if (desired.kind == "absent") {
      plan.push_back(Plan(path, to_remote ? "deleteRemote" : "deleteLocal", l, r, desired));
    } else
      plan.push_back(Plan(path, to_remote ? "upsertRemote" : "upsertLocal", l, r, desired));
  }

  void AddRequiredParents(std::vector<Operation> &plan) {
    std::set<std::pair<std::string, std::string>> existing;
    for (const auto &op : plan) existing.emplace(op.path, op.action);
    const auto count = plan.size();
    for (size_t index = 0; index < count; ++index) {
      const auto original = plan[index];
      if (original.action != "upsertLocal" && original.action != "upsertRemote" &&
          original.action != "mkdirLocal" && original.action != "mkdirRemote")
        continue;
      const bool to_remote = IsRemoteAction(original);
      auto parent = PathFromUtf8(original.path).parent_path();
      while (!parent.empty()) {
        const auto path = PathToGenericUtf8(parent);
        const auto target = Lookup(to_remote ? remote : local, path);
        if (target.kind == "file") Collision();
        if (target.kind == "absent") {
          const std::string action = to_remote ? "mkdirRemote" : "mkdirLocal";
          if (existing.emplace(path, action).second) {
            Version directory;
            directory.kind = "collection";
            plan.push_back(
                Plan(path, action, Lookup(local, path), Lookup(remote, path), directory));
          }
        }
        parent = parent.parent_path();
      }
    }
  }

  void Analyze(WebDavTransport &transport) {
    Progress(SyncState::kAnalyzing, "Comparing WebDAV revisions", 0);
    std::set<std::string> paths;
    for (const auto &entry : local) paths.insert(entry.first);
    for (const auto &entry : remote) paths.insert(entry.first);
    for (const auto &entry : state.entries)
      if (!IsExcluded(entry.first, config)) paths.insert(entry.first);
    for (const auto &entry : state.conflicts) paths.insert(entry.first);
    std::vector<Operation> plan;
    std::vector<SyncFileInfo> view;
    bool conflicts = false;
    common.clear();
    for (const auto &path : paths) {
      CheckCancelled(token);
      const auto l = Lookup(local, path), r = Lookup(remote, path), b = Lookup(state.entries, path);
      if ((l.kind == "file" && r.kind == "collection") ||
          (l.kind == "collection" && r.kind == "file"))
        Collision();
      if (l.kind == "collection" || r.kind == "collection" || b.kind == "collection") {
        // Empty collection retention is intentional: only file deletions propagate remotely.
        if (l.kind == "absent" && r.kind == "collection" && b.kind != "collection")
          AddPlan(transport, plan, path, false, r);
        else if (r.kind == "absent" && l.kind == "collection" && b.kind != "collection")
          AddPlan(transport, plan, path, true, l);
        else if (l.kind == "collection" && r.kind == "absent" && b.kind == "collection")
          plan.push_back(Plan(path, "removeLocalCollection", l, r, {}));
        continue;
      }
      auto incident = state.conflicts.find(path);
      if (incident != state.conflicts.end()) {
        const auto &choice = incident->second;
        const bool valid =
            Same(l, choice.local) && Same(r, choice.remote) && r.etag == choice.remote.etag;
        if (choice.resolution.empty() || !valid) {
          ConflictAt(&transport, path);
          conflicts = true;
          view.push_back({path, SyncFileStatus::kConflicted});
          continue;
        }
        if (choice.resolution == "keep_both" && l.kind == "file" && r.kind == "file" &&
            !Same(l, r)) {
          EnsureLocalSnapshot(path);
          EnsureRemoteSnapshot(transport, path);
          const auto copy_path = ConflictName(path);
          const auto surviving_remote = remote.at(path);
          plan.push_back(Plan(copy_path, "upsertRemote", {}, {}, surviving_remote));
          plan.push_back(Plan(copy_path, "upsertLocal", {}, {}, surviving_remote));
          AddPlan(transport, plan, path, true, Lookup(local, path));
        } else if (choice.resolution == "keep_remote" ||
                   (choice.resolution == "keep_both" && l.kind == "absent")) {
          AddPlan(transport, plan, path, false, r);
        } else
          AddPlan(transport, plan, path, true, l);
        continue;
      }
      if (Same(l, r)) {
        // No payload copies or GETs for unchanged strong ETags. Apply acknowledges only
        // versions still present locally; newer local work keeps its previous baseline.
        if (l.kind == "file") common[path] = r;
        continue;
      }
      if (Same(l, b)) {
        AddPlan(transport, plan, path, false, r);
        view.push_back({path, r.kind == "absent"   ? SyncFileStatus::kDeletedRemote
                              : b.kind == "absent" ? SyncFileStatus::kAddedRemote
                                                   : SyncFileStatus::kModifiedRemote});
      } else if (Same(r, b)) {
        AddPlan(transport, plan, path, true, l);
        view.push_back({path, l.kind == "absent"   ? SyncFileStatus::kDeletedLocal
                              : b.kind == "absent" ? SyncFileStatus::kAddedLocal
                                                   : SyncFileStatus::kModifiedLocal});
      } else {
        ConflictAt(&transport, path);
        conflicts = true;
        view.push_back({path, SyncFileStatus::kConflicted});
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      status = std::move(view);
    }
    state.Save();
    Require(!conflicts, VXCORE_ERR_SYNC_CONFLICT);
    std::set<std::string> retired_scratch;
    for (const auto &previous : state.operations) {
      const auto name = previous.scratch_url.empty() && IsScratch(previous.path)
                            ? previous.path
                            : previous.scratch_url;
      if (name.empty() || !retired_scratch.insert(name).second) continue;
      const auto actual = Inspect(transport, name);
      if (actual.kind == "absent") continue;
      const auto expected_hash =
          previous.scratch_url.empty() ? previous.old_sha256 : previous.new_sha256;
      Require(actual.kind == "file" && actual.sha256 == expected_hash);
      // Carry ownership into the replacement intent, including its verified body. A
      // different resolution must not orphan a previously uploaded complete scratch.
      plan.push_back(Plan(name, "deleteRemote", {}, actual, {}));
    }
    AddRequiredParents(plan);
    SortOperations(plan);
    state.operations = std::move(plan);
    state.Begin();
    state.SavePending();
    // Choices remain durable until both revisions have been confirmed.
  }

  void RecordRace(WebDavTransport &transport, const std::string &path) {
    Require(!IsScratch(path), VXCORE_ERR_INVALID_STATE);
    const auto actual = Inspect(transport, path);
    if (actual.kind == "absent")
      remote.erase(path);
    else
      remote[path] = actual;
    ConflictAt(&transport, path, true);
    state.Save();
    throw Failure{VXCORE_ERR_SYNC_CONFLICT};
  }

  void VerifyRemoteResult(WebDavTransport &transport, Operation &op) {
    auto actual = Inspect(transport, op.path);
    const bool deleted = op.action == "deleteRemote";
    const bool match = deleted ? actual.kind == "absent"
                       : op.kind == "collection"
                           ? actual.kind == "collection"
                           : actual.kind == "file" && actual.sha256 == op.new_sha256;
    if (!match) RecordRace(transport, op.path);
    if (actual.kind == "absent")
      remote.erase(op.path);
    else
      remote[op.path] = std::move(actual);
    op.stage = "remoteConfirmed";
    state.SavePending();
    if (!op.scratch_url.empty()) {
      auto scratch = Inspect(transport, op.scratch_url);
      if (scratch.kind != "absent") {
        // A lost acknowledgement can leave a complete owned scratch beside an already
        // published equal destination. Never delete anything except its journaled bytes.
        Require(scratch.kind == "file" && scratch.sha256 == op.new_sha256);
        Check(transport.RemoveFile(op.scratch_url, scratch.etag));
        Require(Inspect(transport, op.scratch_url, false).kind == "absent");
      }
    }
  }

  void RemoteOperation(WebDavTransport &transport, Operation &op) {
    CheckCancelled(token);
    if (!IsRemoteAction(op)) return;
    auto actual = Inspect(transport, op.path);
    const bool intended = op.action == "deleteRemote" ? actual.kind == "absent"
                          : op.action == "mkdirRemote"
                              ? actual.kind == "collection"
                              : actual.kind == "file" && actual.sha256 == op.new_sha256;
    if (intended) {
      VerifyRemoteResult(transport, op);
      return;
    }
    const bool expected = actual.kind == op.expected_remote_kind &&
                          (actual.kind != "file" || actual.etag == op.expected_remote_etag);
    if (!expected || op.stage != "prepared") RecordRace(transport, op.path);
    if (op.action == "mkdirRemote") {
      Check(transport.MakeCollection(op.path));
      VerifyRemoteResult(transport, op);
      return;
    }
    if (op.action == "deleteRemote") {
      Require(actual.kind == "file");
      state.VerifySnapshot(op.previous_snapshot, op.old_sha256);
      const auto result = transport.RemoveFile(op.path, op.expected_remote_etag);
      if (result == VXCORE_ERR_SYNC_CONFLICT) RecordRace(transport, op.path);
      Check(result);
      VerifyRemoteResult(transport, op);
      return;
    }
    state.VerifySnapshot(op.source_snapshot, op.new_sha256);
    if (!op.previous_snapshot.empty()) state.VerifySnapshot(op.previous_snapshot, op.old_sha256);
    Require(!op.scratch_url.empty());
    auto scratch = Inspect(transport, op.scratch_url);
    if (scratch.kind == "absent") {
      // The complete intent, owned name and payload are durable before PUT.
      Check(transport.Upload(op.scratch_url, state.directory, op.source_snapshot, ""));
      scratch = Inspect(transport, op.scratch_url);
    }
    Require(scratch.kind == "file" && scratch.sha256 == op.new_sha256, VXCORE_ERR_INVALID_STATE);
    state.SavePending();
    const auto result =
        transport.Move(op.scratch_url, scratch.etag, op.path, op.expected_remote_etag);
    if (result == VXCORE_ERR_SYNC_CONFLICT) RecordRace(transport, op.path);
    Check(result);
    VerifyRemoteResult(transport, op);
  }

  void ValidateCohort() {
    // Validate the final metadata cohort, not merely each JSON document in isolation.
    std::map<std::string, fs::path> metadata;
    for (const auto &entry : local)
      if (entry.second.kind == "file" && IsMetadata(entry.first)) {
        fs::path path;
        Check(WebDavTransport::ResolveLocalPath(state.root, entry.first, path));
        metadata[entry.first] = path;
      }
    for (const auto &op : state.operations) {
      if (!IsMetadata(op.path) && op.path != "vx_notebook/encryption.vne") continue;
      if (op.action == "upsertLocal" || op.action == "upsertRemote")
        metadata[op.path] = state.Resolve(op.source_snapshot);
      else if (op.action == "deleteLocal" || op.action == "deleteRemote")
        metadata.erase(op.path);
    }
    Require(metadata.count(kConfigPath));
    std::vector<std::string> ids;
    for (const auto &entry : metadata)
      ValidateMetadata(entry.first, entry.second, state.notebook_id, &ids);
    std::set<std::string> unique;
    for (const auto &id : ids) Require(unique.insert(id).second);
  }

  void Bootstrap(WebDavTransport &transport) {
    if (!bootstrap) return;
    VerifyIdentity(transport, true);
    if (!bootstrap) return;
    auto config_op = std::find_if(
        state.operations.begin(), state.operations.end(),
        [](const Operation &op) { return op.path == kConfigPath && op.action == "upsertRemote"; });
    Require(config_op != state.operations.end());
    auto parent = std::find_if(
        state.operations.begin(), state.operations.end(),
        [](const Operation &op) { return op.path == "vx_notebook" && op.action == "mkdirRemote"; });
    if (parent != state.operations.end()) RemoteOperation(transport, *parent);
    // The existing dedicated root is claimed by config first; no other user payload precedes it.
    RemoteOperation(transport, *config_op);
    bootstrap = false;
  }

  void Recover(WebDavTransport &transport) {
    bool conflict = false;
    for (auto &op : state.operations) {
      CheckCancelled(token);
      Require(!IsScratch(op.path) ||
              (op.action == "deleteRemote" && op.expected_local_kind == "absent" &&
               !op.previous_snapshot.empty()));
      auto l = Lookup(local, op.path), r = Lookup(remote, op.path);
      const bool local_expected = l.kind == op.expected_local_kind &&
                                  (l.kind != "file" || l.raw_sha256 == op.expected_local_sha256);
      const bool local_intended =
          op.action == "deleteLocal" || op.action == "removeLocalCollection" ? l.kind == "absent"
          : op.action == "mkdirLocal"
              ? l.kind == "collection"
              : !op.new_sha256.empty() && l.kind == "file" && l.sha256 == op.new_sha256;
      bool remote_expected = r.kind == op.expected_remote_kind &&
                             (r.kind != "file" || r.etag == op.expected_remote_etag);
      bool remote_intended = op.action == "deleteRemote" || op.action == "deleteLocal" ||
                                     op.action == "removeLocalCollection"
                                 ? r.kind == "absent"
                             : op.kind == "collection"
                                 ? r.kind == "collection"
                                 : r.kind == "file" && r.sha256 == op.new_sha256;
      // A Keep Both duplicate has an ordered upload followed by local installation.
      if (!remote_intended && IsLocalAction(op)) {
        remote_expected = std::any_of(
            state.operations.begin(), state.operations.end(), [&](const Operation &other) {
              return other.path == op.path && other.action == "upsertRemote" &&
                     other.new_sha256 == op.new_sha256 && remote_expected;
            });
      }
      if ((!local_expected && !local_intended) ||
          (IsRemoteAction(op) ? !remote_expected && !remote_intended
                              : !remote_intended && !remote_expected)) {
        ConflictAt(&transport, op.path);
        conflict = true;
      }
    }
    // Continue the full comparison before persisting every discovered conflict.
    std::set<std::string> planned;
    for (const auto &op : state.operations) planned.insert(op.path);
    std::set<std::string> paths;
    for (const auto &entry : local) paths.insert(entry.first);
    for (const auto &entry : remote) paths.insert(entry.first);
    for (const auto &entry : state.entries)
      if (!IsExcluded(entry.first, config)) paths.insert(entry.first);
    std::vector<Operation> additions;
    for (const auto &path : paths) {
      const auto l = Lookup(local, path), r = Lookup(remote, path), b = Lookup(state.entries, path);
      if ((l.kind == "file" && r.kind == "collection") ||
          (l.kind == "collection" && r.kind == "file"))
        Collision();
      if (planned.count(path)) continue;
      if (l.kind == "collection" || r.kind == "collection" || b.kind == "collection") {
        if (l.kind == "absent" && r.kind == "collection" && b.kind != "collection")
          AddPlan(transport, additions, path, false, r);
        else if (r.kind == "absent" && l.kind == "collection" && b.kind != "collection")
          AddPlan(transport, additions, path, true, l);
        else if (l.kind == "collection" && r.kind == "absent" && b.kind == "collection")
          additions.push_back(Plan(path, "removeLocalCollection", l, r, {}));
      } else if (Same(l, r)) {
        if (l.kind == "file") common[path] = r;
      } else if (Same(l, b))
        AddPlan(transport, additions, path, false, r);
      else if (Same(r, b))
        AddPlan(transport, additions, path, true, l);
      else {
        ConflictAt(&transport, path);
        conflict = true;
      }
    }
    if (conflict) {
      state.Save();
      throw Failure{VXCORE_ERR_SYNC_CONFLICT};
    }
    for (auto &op : additions) state.operations.push_back(std::move(op));
    AddRequiredParents(state.operations);
    SortOperations(state.operations);
    state.SavePending();
  }

  bool JournalContainsChoices() const {
    for (const auto &incident : state.conflicts) {
      const auto &choice = incident.second;
      const bool use_remote = choice.resolution == "keep_remote" ||
                              (choice.resolution == "keep_both" && choice.local.kind == "absent");
      const auto &desired = use_remote ? choice.remote : choice.local;
      const auto action = desired.kind == "absent" ? (use_remote ? "deleteLocal" : "deleteRemote")
                                                   : (use_remote ? "upsertLocal" : "upsertRemote");
      const auto match =
          std::find_if(state.operations.begin(), state.operations.end(), [&](const Operation &op) {
            return op.path == incident.first && op.action == action &&
                   op.new_sha256 == desired.sha256 &&
                   op.expected_remote_etag == choice.remote.etag &&
                   op.expected_local_kind == choice.local.kind &&
                   (choice.local.kind != "file" ||
                    op.expected_local_sha256 == choice.local.sha256 ||
                    (incident.first == kConfigPath &&
                     op.expected_local_sha256 == Lookup(local, incident.first).raw_sha256));
          });
      if (match == state.operations.end()) return false;
      if (choice.resolution == "keep_both" && choice.local.kind == "file" &&
          choice.remote.kind == "file" && !Same(choice.local, choice.remote)) {
        const bool duplicate =
            std::any_of(state.operations.begin(), state.operations.end(), [&](const Operation &op) {
              return op.path != incident.first && op.action == "upsertLocal" &&
                     op.new_sha256 == choice.remote.sha256 &&
                     op.path.find(".sync-conflict-") != std::string::npos;
            });
        if (!duplicate) return false;
      }
    }
    return true;
  }

  void Exchange() {
    Require(initialized.load() && prepared, VXCORE_ERR_INVALID_STATE);
    exchanged = false;
    Progress(SyncState::kFetching, "Reading WebDAV notebook", 0);
    std::string username;
    auto transport = Session(username, true);
    Require(transport->CanonicalRoot() == state.remote_url);
    VerifyIdentity(*transport, true);
    verified_username = username;
    ListRemote(*transport, true);
    if (state.operations.empty()) state.RotateUsername(username);
    bool all_resolved = !state.conflicts.empty();
    for (const auto &c : state.conflicts)
      all_resolved = all_resolved && !c.second.resolution.empty();
    if (!state.operations.empty() && all_resolved && !JournalContainsChoices()) {
      // The old intent remains on disk until a fully validated replacement has been written.
      Analyze(*transport);
    } else if (!state.operations.empty()) {
      Require(state.conflicts.empty() || all_resolved, VXCORE_ERR_SYNC_CONFLICT);
      Recover(*transport);
    } else
      Analyze(*transport);
    ValidateCohort();
    CheckCancelled(token);
    Bootstrap(*transport);
    Progress(SyncState::kPushing, "Publishing WebDAV revisions", 0);
    for (auto &op : state.operations) RemoteOperation(*transport, op);
    // Revalidate every deferred remote representation after outgoing mutations. Apply itself
    // is network-free and uses this verified comparison snapshot.
    for (auto &op : state.operations) {
      if (!IsLocalAction(op)) continue;
      auto actual = Inspect(*transport, op.path);
      bool match = op.action == "deleteLocal" || op.action == "removeLocalCollection"
                       ? actual.kind == "absent"
                   : op.kind == "collection"
                       ? actual.kind == "collection"
                       : actual.kind == "file" && actual.sha256 == op.new_sha256;
      if (!match) RecordRace(*transport, op.path);
      if (actual.kind == "absent")
        remote.erase(op.path);
      else
        remote[op.path] = std::move(actual);
      op.stage = "remoteConfirmed";
      state.SavePending();
    }
    exchanged = true;
  }

  void Apply(const std::vector<std::string> &protected_paths, std::vector<std::string> &changed) {
    Require(initialized.load() && prepared && exchanged, VXCORE_ERR_INVALID_STATE);
    for (const auto &path : protected_paths) Check(WebDavTransport::ValidateRelativePath(path));
    CheckCancelled(token);
    Progress(SyncState::kMerging, "Installing WebDAV revisions", 0);
    bool conflict = false;
    // Preflight the ENTIRE cohort before changing a single working file.
    for (const auto &op : state.operations) {
      auto actual = ReadLocal(state.root, op.path, state.notebook_id, token);
      const bool expected =
          actual.kind == op.expected_local_kind &&
          (actual.kind != "file" || actual.raw_sha256 == op.expected_local_sha256);
      const bool intended =
          op.action == "deleteLocal" || op.action == "deleteRemote" ? actual.kind == "absent"
          : op.kind == "collection"                                 ? actual.kind == "collection"
                                    : actual.kind == "file" && actual.sha256 == op.new_sha256;
      const bool modifies = IsLocalAction(op) && !intended;
      if ((!expected && !intended) || (modifies && Protected(op.path, protected_paths))) {
        // Remote snapshots were retained by Exchange; no network occurs under the IO gate.
        ConflictAt(nullptr, op.path, true);
        conflict = true;
      }
    }
    if (conflict) {
      state.Save();
      throw Failure{VXCORE_ERR_SYNC_CONFLICT};
    }
    // A Keep Both path has a remote publication followed by local installation. Do not
    // advance that path's baseline at the earlier remote-only journal row.
    ValidateCohort();
    CheckCancelled(token);
    for (auto &op : state.operations) {
      CheckCancelled(token);
      auto actual = ReadLocal(state.root, op.path, state.notebook_id, token);
      fs::path destination;
      Check(WebDavTransport::ResolveLocalPath(state.root, op.path, destination));
      bool installed = false;
      if (op.action == "upsertLocal" &&
          !(actual.kind == "file" && actual.sha256 == op.new_sha256)) {
        state.VerifySnapshot(op.source_snapshot, op.new_sha256);
        if (actual.kind == "file") {
          // The projected config snapshot is not a backup of routing bytes. Preserve actual
          // on-disk bytes exclusively before a local replacement, and journal that backup.
          op.previous_snapshot = state.SnapshotFile(destination, token);
          op.old_sha256 = actual.raw_sha256;
          state.SavePending();
        }
        if (op.path == kConfigPath) {
          const auto current = NotebookJson(destination, state.notebook_id);
          const auto bytes = RestoreRouting(ReadBytes(state.Resolve(op.source_snapshot)), current);
          Check(WriteFileAtomic(destination, bytes));
        } else
          CopyAtomic(state.Resolve(op.source_snapshot), destination, token);
        installed = true;
      } else if (op.action == "deleteLocal" && actual.kind != "absent") {
        Require(actual.kind == "file");
        op.previous_snapshot = state.SnapshotFile(destination, token);
        op.old_sha256 = actual.raw_sha256;
        state.SavePending();
        state.VerifySnapshot(op.previous_snapshot, op.old_sha256);
        std::error_code error;
        Require(fs::remove(destination, error) && !error, VXCORE_ERR_IO);
        installed = true;
      } else if (op.action == "mkdirLocal" && actual.kind == "absent") {
        std::error_code error;
        Require(fs::create_directory(destination, error) && !error, VXCORE_ERR_IO);
        installed = true;
      } else if (op.action == "removeLocalCollection" && actual.kind == "collection") {
        std::error_code error;
        if (fs::is_empty(destination, error) && !error) {
          Require(fs::remove(destination, error) && !error, VXCORE_ERR_IO);
          installed = true;
        }
      }
      if (installed) changed.push_back(op.path);
      // Once a sibling replacement is committed, acknowledge that bounded step even if
      // cancellation arrived during the commit. The next iteration observes cancellation.
      op.stage = "localConfirmed";
      state.SavePending();
      if (IsRemoteAction(op) && std::any_of(state.operations.begin(), state.operations.end(),
                                            [&](const Operation &other) {
                                              return other.path == op.path && IsLocalAction(other);
                                            }))
        continue;
      const auto confirmed = ReadLocal(state.root, op.path, state.notebook_id, nullptr);
      const auto remote_version = Lookup(remote, op.path);
      if (op.action == "removeLocalCollection" && confirmed.kind == "collection") {
        // A concurrent/excluded child prevents nonrecursive removal; retain the container.
      } else
        Require(Same(confirmed, remote_version), VXCORE_ERR_SYNC_CONFLICT);
      if (remote_version.kind == "absent")
        state.entries.erase(op.path);
      else
        state.entries[op.path] = BaselineVersion(remote_version);
      state.conflicts.erase(op.path);
      state.Save();
      op.stage = "baselineConfirmed";
      state.SavePending();
    }
    for (const auto &entry : common) {
      const auto actual = ReadLocal(state.root, entry.first, state.notebook_id, token);
      if (Same(actual, entry.second)) state.entries[entry.first] = BaselineVersion(entry.second);
    }
    // Collections retained on either side are not file deletion facts.
    for (const auto &entry : remote)
      if (entry.second.kind == "collection")
        state.entries[entry.first] = BaselineVersion(entry.second);
    for (auto it = state.entries.begin(); it != state.entries.end();) {
      if (!IsExcluded(it->first, config) && it->second.kind == "file" && !local.count(it->first) &&
          !remote.count(it->first))
        it = state.entries.erase(it);
      else
        ++it;
    }
    state.Save();
    const auto completed = state.operations;
    state.ClearPending();
    state.RotateUsername(verified_username);
    state.RemoveUnreferencedSnapshots(completed);
    state.CleanupTransient();
    prepared = false;
    exchanged = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      status.clear();
    }
    Progress(SyncState::kIdle, "WebDAV synchronization complete", 100);
  }

  void Clone(const std::string &target, const SyncConfig &cfg) {
    initialized.store(false);
    prepared = false;
    exchanged = false;
    config = cfg;
    Require(config.backend == "webdav", VXCORE_ERR_INVALID_PARAM);
    const auto root = fs::absolute(PathFromUtf8(target));
    Require(fs::is_directory(root) && fs::is_empty(root), VXCORE_ERR_INVALID_PARAM);
    Require(CheckReparsePoint(PathToUtf8(root)) == ReparseState::kNo, VXCORE_ERR_UNSUPPORTED);
    std::string username;
    auto transport = Session(username, false);
    std::vector<WebDavResource> resources;
    Check(transport->List(resources));
    auto identity = std::find_if(resources.begin(), resources.end(), [](const WebDavResource &r) {
      return r.path == kConfigPath && r.kind == WebDavResourceKind::kFile;
    });
    Require(identity != resources.end(), VXCORE_ERR_INVALID_STATE);
    // The staging root is caller-owned and initially empty; downloading config here cannot
    // replace user content. No writable runtime or capability probe is created by Clone.
    EnsureDirectory(root, "vx_notebook");
    Check(transport->Download(kConfigPath, identity->etag, root, kConfigPath));
    const auto notebook = NotebookJson(root / PathFromUtf8(kConfigPath));
    state.Open(root, notebook.at(kJsonKeyId).get<std::string>(), transport->CanonicalRoot(),
               username);
    Tree downloaded;
    std::sort(resources.begin(), resources.end(),
              [](const WebDavResource &a, const WebDavResource &b) {
                if (a.kind != b.kind) return a.kind == WebDavResourceKind::kCollection;
                return a.path.size() < b.path.size();
              });
    for (const auto &r : resources) {
      CheckCancelled(token);
      if (r.path.empty() || IsScratch(r.path) || IsExcluded(r.path, config)) continue;
      fs::path file;
      Check(WebDavTransport::ResolveLocalPath(root, r.path, file));
      Version v;
      if (r.kind == WebDavResourceKind::kCollection) {
        EnsureDirectory(root, r.path);
        v.kind = "collection";
      } else {
        if (r.path != kConfigPath) Check(transport->Download(r.path, r.etag, root, r.path));
        ValidateMetadata(r.path, file, state.notebook_id);
        v = ReadLocal(root, r.path, state.notebook_id, token);
        v.etag = r.etag;
        if (r.path == kConfigPath) {
          // A clone starts with no device routing, even if an external client supplied it.
          Check(WriteFileAtomic(file, ProjectConfig(NotebookJson(file, state.notebook_id))));
        }
      }
      downloaded.emplace(r.path, std::move(v));
    }
    std::vector<WebDavResource> verified;
    Check(transport->List(verified));
    Tree final;
    for (const auto &r : verified) {
      if (r.path.empty() || IsScratch(r.path) || IsExcluded(r.path, config)) continue;
      auto before = downloaded.find(r.path);
      Require(before != downloaded.end(), VXCORE_ERR_SYNC_CONFLICT);
      Require(
          (r.kind == WebDavResourceKind::kFile ? "file" : "collection") == before->second.kind &&
              (r.kind != WebDavResourceKind::kFile || r.etag == before->second.etag),
          VXCORE_ERR_SYNC_CONFLICT);
      final.emplace(r.path, before->second);
    }
    Require(final.size() == downloaded.size(), VXCORE_ERR_SYNC_CONFLICT);
    local = downloaded;
    remote = downloaded;
    ValidateCohort();
    state.entries.clear();
    for (const auto &entry : downloaded)
      state.entries.emplace(entry.first, BaselineVersion(entry.second));
    state.Save();
  }
};

WebDavSyncBackend::WebDavSyncBackend(const SyncConfig &config,
                                     std::shared_ptr<ICredentialProvider> provider)
    : impl_(std::make_unique<Impl>(config, std::move(provider))) {}
WebDavSyncBackend::~WebDavSyncBackend() = default;
std::string WebDavSyncBackend::GetName() const { return "webdav"; }
SyncCapabilities WebDavSyncBackend::GetCapabilities() const {
  return static_cast<uint32_t>(SyncCapability::ConflictDetection) |
         static_cast<uint32_t>(SyncCapability::ConflictResolution) |
         static_cast<uint32_t>(SyncCapability::IncrementalSync) |
         static_cast<uint32_t>(SyncCapability::AuthRequired) |
         static_cast<uint32_t>(SyncCapability::Cancellation) |
         static_cast<uint32_t>(SyncCapability::ProgressReporting) |
         static_cast<uint32_t>(SyncCapability::Cloneable) |
         static_cast<uint32_t>(SyncCapability::DeferredLocalApply);
}
bool WebDavSyncBackend::IsInitialized() const { return impl_->initialized.load(); }
VxCoreError WebDavSyncBackend::Initialize(const std::string &root, const SyncConfig &config) {
  impl_->initialized.store(false);
  return impl_->Run([&] { impl_->Initialize(root, config); });
}
VxCoreError WebDavSyncBackend::Clone(const std::string &target, const SyncConfig &config) {
  return impl_->Run([&] { impl_->Clone(target, config); });
}
void WebDavSyncBackend::ReplaceCredsProvider(std::shared_ptr<ICredentialProvider> provider) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->provider = std::move(provider);
}
std::shared_ptr<ICredentialProvider> WebDavSyncBackend::GetCredsProviderSnapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->provider;
}
void WebDavSyncBackend::SetCancellation(SyncCancellationPtr token) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->cancellation = std::move(token);
}
VxCoreError WebDavSyncBackend::Sync(SyncProgressCallback callback, void *userdata) {
  return impl_->Run([&] {
    impl_->callback = std::move(callback);
    impl_->userdata = userdata;
    struct ClearCallback {
      Impl &impl;
      ~ClearCallback() {
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
VxCoreError WebDavSyncBackend::StageAndCommit(bool *out_did_commit) {
  if (out_did_commit) *out_did_commit = false;
  return impl_->Run([&] { impl_->Stage(); });
}
VxCoreError WebDavSyncBackend::FetchRebasePush() {
  return impl_->Run([&] { impl_->Exchange(); });
}
VxCoreError WebDavSyncBackend::ApplySync(const std::vector<std::string> &protected_paths,
                                         std::vector<std::string> &out_changed_paths) {
  out_changed_paths.clear();
  return impl_->Run([&] { impl_->Apply(protected_paths, out_changed_paths); });
}
VxCoreError WebDavSyncBackend::GetStatus(std::vector<SyncFileInfo> &out_files) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  out_files = impl_->status;
  return VXCORE_OK;
}
VxCoreError WebDavSyncBackend::GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  out_conflicts = impl_->public_conflicts;
  return VXCORE_OK;
}
VxCoreError WebDavSyncBackend::ResolveConflict(const std::string &path,
                                               SyncConflictResolution resolution) {
  return impl_->Run([&] {
    Require(impl_->initialized.load(), VXCORE_ERR_INVALID_STATE);
    Check(WebDavTransport::ValidateRelativePath(path));
    auto it = impl_->state.conflicts.find(path);
    Require(it != impl_->state.conflicts.end(), VXCORE_ERR_NOT_FOUND);
    Require(resolution == SyncConflictResolution::kKeepLocal ||
                resolution == SyncConflictResolution::kKeepRemote ||
                resolution == SyncConflictResolution::kKeepBoth,
            VXCORE_ERR_INVALID_PARAM);
    Require(resolution != SyncConflictResolution::kKeepBoth || it->second.can_keep_both,
            VXCORE_ERR_UNSUPPORTED);
    const auto previous = it->second.resolution;
    it->second.resolution = resolution == SyncConflictResolution::kKeepLocal    ? "keep_local"
                            : resolution == SyncConflictResolution::kKeepRemote ? "keep_remote"
                                                                                : "keep_both";
    try {
      impl_->state.Save();
    } catch (...) {
      it->second.resolution = previous;
      throw;
    }
  });
}
std::string WebDavSyncBackend::GetLastError() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->last_error;
}

}  // namespace vxcore
