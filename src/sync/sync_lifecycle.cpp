#include "sync_lifecycle.h"

#include <filesystem>
#include <initializer_list>
#include <new>
#include <string_view>
#include <utility>

#include "core/config_manager.h"
#include "core/context.h"
#include "core/notebook.h"
#include "core/notebook_manager.h"
#include "sync/credential_provider.h"
#include "sync/http/sync_http_client.h"
#include "sync/sync_backend_registry.h"
#include "sync/sync_manager.h"
#include "sync/sync_provider_retirement.h"
#include "sync/webdav/webdav_state.h"
#include "sync/webdav/webdav_transport.h"

namespace vxcore {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using webdav::Check;
using webdav::Require;

void Closed(const Json &object, std::initializer_list<std::string_view> allowed) {
  Require(object.is_object(), VXCORE_ERR_INVALID_PARAM);
  for (auto item = object.begin(); item != object.end(); ++item) {
    bool known = false;
    for (const auto key : allowed) known = known || std::string_view(item.key()) == key;
    Require(known, VXCORE_ERR_INVALID_PARAM);
  }
}
std::string Uuid(const Json &value) {
  Require(value.is_string(), VXCORE_ERR_INVALID_PARAM);
  const auto id = value.get<std::string>();
  Require(EncryptedNote::IsCanonicalUuid(id), VXCORE_ERR_INVALID_PARAM);
  return id;
}
void NoFollow(const fs::path &path) {
  private_io::Validate(path);
}
std::string BaselineHash(const std::string &root) {
  const auto path = PathFromUtf8(root) / webdav::kPrivatePath / "state.json";
  const auto status = private_io::Status(path);
  if (!status.exists) return {};
  Require(status.regular, VXCORE_ERR_INVALID_STATE);
  return webdav::HashFile(path, nullptr, FileAccess::kPrivate);
}
SyncConfig ParseConfig(const Json &json) {
  Closed(json, {"backend", "remoteUrl", "autoSyncEnabled"});
  Require(json.at("backend") == "webdav", VXCORE_ERR_UNSUPPORTED);
  Require(json.at("remoteUrl").is_string(), VXCORE_ERR_INVALID_PARAM);
  if (json.contains("autoSyncEnabled"))
    Require(json.at("autoSyncEnabled").is_boolean(), VXCORE_ERR_INVALID_PARAM);
  SyncConfig config = SyncConfig::FromJson(json);
  sync_http::Url url;
  Require(sync_http::ParseUrl(config.remote_url, url) && url.scheme == "https",
          VXCORE_ERR_INVALID_PARAM);
  return config;
}
uint64_t Limit(const Json &json, const char *name) {
  const auto &value = json.at(name);
  Require(value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>() > 0),
          VXCORE_ERR_INVALID_PARAM);
  const auto bound = value.get<uint64_t>();
  Require(bound > 0, VXCORE_ERR_INVALID_PARAM);
  return bound;
}
void RequireEmptyContext(VxCoreContext &context) {
  std::string notebooks;
  Check(context.notebook_manager->ListNotebooks(notebooks));
  auto list = Json::parse(notebooks);
  Require(list.is_array() && list.empty(), VXCORE_ERR_INVALID_STATE);
}
void RequirePrivateClone(VxCoreContext &context, const std::string &target) {
  // Shared owners are categorically ineligible, regardless of whether a caller
  // has created the private runtime sibling paths yet.
  RequireEmptyContext(context);
  const auto path = PathFromUtf8(target);
  NoFollow(path);
  Require(private_io::IsDirectory(path), VXCORE_ERR_INVALID_PARAM);
  Require(path.filename() == "unpacked" &&
          path.parent_path().parent_path().filename() == "imports", VXCORE_ERR_INVALID_PARAM);
  const auto runtime = path.parent_path() / "runtime";
  const auto app = PathFromUtf8(context.config_manager->GetAppDataPath());
  const auto local = PathFromUtf8(context.config_manager->GetLocalDataPath());
#if defined(_WIN32)
  // Complete still uses the ordinary notebook/session/SQLite owner. Admit its
  // fixed paths before any worker runs, without restricting private descendants.
  // The representative UUID is length-only; never query or persist that path.
  const auto journal = local / "notebooks" / "00000000-0000-4000-8000-000000000000" /
                       "metadata.db-journal";
  Require(path.native().size() < 260 &&
          (path / webdav::kConfigPath).native().size() < 260 &&
          journal.native().size() < 260 &&
          (local / "vxsession.json").native().size() < 260, VXCORE_ERR_UNSUPPORTED);
#endif
  NoFollow(app);
  NoFollow(local);
  FileIdentity app_identity, local_identity, expected_app, expected_local;
  Check(GetFileIdentity(app, app_identity, FileAccess::kPrivate));
  Check(GetFileIdentity(local, local_identity, FileAccess::kPrivate));
  Check(GetFileIdentity(runtime / "app", expected_app, FileAccess::kPrivate));
  Check(GetFileIdentity(runtime / "local", expected_local, FileAccess::kPrivate));
  Require(app_identity.SameObject(expected_app) && local_identity.SameObject(expected_local),
          VXCORE_ERR_INVALID_STATE);
}
}  // namespace

VxCoreError SyncLifecycle::Prepare(VxCoreContext &context, const char *notebook_id,
                                   const Json &command, SyncCredentials credentials,
                                   std::unique_ptr<SyncLifecycle> &out) {
  out.reset();
  try {
    Require(context.sync_manager && context.notebook_manager && context.config_manager,
            VXCORE_ERR_INVALID_STATE);
    Require(command.is_object() && command.at("version").is_number_integer() &&
            command.at("version") == 1 && command.at("kind").is_string(),
            VXCORE_ERR_INVALID_PARAM);
    auto task = std::unique_ptr<SyncLifecycle>(new SyncLifecycle());
    task->owner_ = &context;
    task->manager_ = context.sync_manager.get();
    task->operation_id_ = Uuid(command.at("operationId"));
    task->kind_ = command.at("kind").get<std::string>();
    const bool clone = task->kind_ == "clone";
    const bool rotation = task->kind_ == "credentials";
    const bool recovery = task->kind_ == "credentialRecovery";
    Require(clone || rotation || recovery || task->kind_ == "enable", VXCORE_ERR_INVALID_PARAM);
    if (clone) {
      Closed(command, {"version", "operationId", "kind", "config", "target", "limits"});
      Require(!notebook_id || !*notebook_id, VXCORE_ERR_INVALID_PARAM);
      Require(command.at("target").is_string(), VXCORE_ERR_INVALID_PARAM);
      task->root_ = command.at("target").get<std::string>();
      RequirePrivateClone(context, task->root_);
      Require(private_io::IsEmpty(PathFromUtf8(task->root_)), VXCORE_ERR_ALREADY_EXISTS);
    } else {
      Require(notebook_id && EncryptedNote::IsCanonicalUuid(notebook_id), VXCORE_ERR_INVALID_PARAM);
      task->notebook_id_ = notebook_id;
      task->lease_key_ = notebook_id;
      task->notebook_ = context.notebook_manager->GetNotebook(notebook_id);
      Require(task->notebook_ != nullptr, VXCORE_ERR_NOT_FOUND);
      Require(task->notebook_->GetType() == NotebookType::Bundled, VXCORE_ERR_UNSUPPORTED);
      Check(task->notebook_->CheckWritable());
      task->root_ = task->notebook_->GetRootFolder();
      NoFollow(PathFromUtf8(task->root_));
      task->owner_config_ = task->notebook_->GetConfig().ToJson().dump();
      const auto config_path = PathFromUtf8(task->root_) / webdav::kConfigPath;
      NoFollow(config_path);
      webdav::NotebookJson(config_path, task->notebook_id_, FileAccess::kPrivate);
      task->config_sha256_ = webdav::HashFile(config_path, nullptr, FileAccess::kPrivate);
      task->baseline_sha256_ = BaselineHash(task->root_);
      if (rotation) {
        Closed(command, {"version", "operationId", "kind"});
        Check(task->manager_->GetSyncConfig(notebook_id, task->config_));
        Require(task->config_.backend == "webdav", VXCORE_ERR_UNSUPPORTED);
      } else if (recovery) {
        Closed(command, {"version", "operationId", "kind", "config", "recoveryOperationId"});
        task->recovery_operation_id_ = Uuid(command.at("recoveryOperationId"));
      } else {
        Closed(command, {"version", "operationId", "kind", "config"});
      }
      Json facts;
      Check(InspectSyncStorage(task->root_, task->notebook_id_, facts));
      if (rotation)
        Require(!facts.at("pending").get<bool>() && !facts.at("conflicts").get<bool>() &&
                !facts.at("probe").get<bool>(), VXCORE_ERR_SYNC_CONFLICT);
      Require(!facts.at("retirement").get<bool>(), VXCORE_ERR_INVALID_STATE);
      if (!recovery)
        Require(!facts.at("credentialValidation").get<bool>(), VXCORE_ERR_INVALID_STATE);
    }
    if (!rotation) task->config_ = ParseConfig(command.at("config"));
    if (clone) {
      task->config_.private_clone = true;
      const auto &limits = command.at("limits");
      Closed(limits, {"maxFileBytes", "maxTotalBytes", "maxEntries", "maxDepth", "maxPathBytes"});
      auto &target = task->config_.clone_limits;
      target.max_file_bytes = Limit(limits, "maxFileBytes");
      target.max_total_bytes = Limit(limits, "maxTotalBytes");
      target.max_entries = Limit(limits, "maxEntries");
      target.max_depth = Limit(limits, "maxDepth");
      target.max_path_bytes = Limit(limits, "maxPathBytes");
      Require(target.max_file_bytes <= target.max_total_bytes && target.max_entries <= 500000 &&
              target.max_depth <= 256 && target.max_path_bytes <= 32768, VXCORE_ERR_INVALID_PARAM);
    }
    {
      std::lock_guard<std::mutex> lock(task->manager_->state_mutex_);
      auto &manager = *task->manager_;
      Require(!manager.RegistrationBlockedLocked(task->lease_key_), VXCORE_ERR_SYNC_IN_PROGRESS);
      const auto old = manager.backends_.find(task->notebook_id_);
      task->previous_backend_ = old == manager.backends_.end() ? nullptr : old->second.get();
      Require(!clone || manager.backends_.empty(), VXCORE_ERR_INVALID_STATE);
      Require(!rotation || task->previous_backend_, VXCORE_ERR_SYNC_NOT_ENABLED);
      Require(task->kind_ != "enable" || !task->previous_backend_, VXCORE_ERR_ALREADY_INITIALIZED);
      const auto phase = manager.deferred_phases_.find(task->notebook_id_);
      Require(phase == manager.deferred_phases_.end(), VXCORE_ERR_SYNC_IN_PROGRESS);
      task->config_.tls_policy = manager.tls_policy_;
      manager.lifecycle_leases_.emplace(task->lease_key_, task.get());
      task->leased_ = true;
    }
    // Creating a session is local validation only. It also canonicalizes the
    // exact endpoint and rejects unsupported custom trust before a worker runs.
    WebDavTransport transport(task->config_.remote_url, credentials, nullptr, task->config_.tls_policy);
    Check(transport.Initialize());
    task->config_.remote_url = transport.CanonicalRoot();
    if (!clone) {
      const auto &persisted = task->notebook_->GetConfig();
      Require(persisted.sync_backend.empty() || persisted.sync_backend == "webdav", VXCORE_ERR_UNSUPPORTED);
      if (!persisted.sync_remote_url.empty()) {
        WebDavTransport current(persisted.sync_remote_url, credentials, nullptr, task->config_.tls_policy);
        Check(current.Initialize());
        Require(current.CanonicalRoot() == task->config_.remote_url, VXCORE_ERR_INVALID_STATE);
      }
    }
    Check(GetFileIdentity(PathFromUtf8(task->root_), task->root_identity_, FileAccess::kPrivate));
    task->provider_ = std::make_shared<InMemoryCredentialProvider>(std::move(credentials));
    task->backend_ = SyncBackendRegistry::Instance().Create("webdav", task->config_, task->provider_);
    Require(task->backend_ != nullptr, VXCORE_ERR_UNKNOWN_BACKEND);
    {
      std::lock_guard<std::mutex> lock(task->manager_->state_mutex_);
      // The live lease already excludes replacement during validation. Only a
      // successfully returned handle permanently freezes the prior TLS policy.
      task->manager_->tls_policy_locked_ = true;
    }
    out = std::move(task);
    return VXCORE_OK;
  } catch (const webdav::Failure &error) { return error.error; }
    catch (const std::bad_alloc &) { return VXCORE_ERR_OUT_OF_MEMORY; }
    catch (const nlohmann::json::exception &) { return VXCORE_ERR_INVALID_PARAM; }
    catch (...) { return VXCORE_ERR_INVALID_STATE; }
}

SyncLifecycle::~SyncLifecycle() {
  // Free is owner-only after join. Never delete on-disk recovery as a side effect.
  if (leased_) {
    std::lock_guard<std::mutex> lock(manager_->state_mutex_);
    const auto it = manager_->lifecycle_leases_.find(lease_key_);
    if (it != manager_->lifecycle_leases_.end() && it->second == this)
      manager_->lifecycle_leases_.erase(it);
  }
}

VxCoreError SyncLifecycle::Execute(SyncCancellationPtr cancellation) noexcept {
  auto expected = Phase::kPrepared;
  if (!phase_.compare_exchange_strong(expected, Phase::kExecuting)) return VXCORE_ERR_INVALID_STATE;
  cancellation_ = std::move(cancellation);
  executed_ = true;
  try {
    webdav::CheckCancelled(cancellation_);
    backend_->SetCancellation(cancellation_);
    if (kind_ == "clone") execution_code_ = backend_->Clone(root_, config_);
    else if (kind_ == "credentialRecovery")
      execution_code_ = backend_->RecoverCandidate(root_, config_, provider_, recovery_operation_id_);
    else execution_code_ = backend_->ValidateCandidate(root_, config_, provider_, operation_id_);
    if (execution_code_ == VXCORE_OK && cancellation_ && cancellation_->IsCancelled())
      execution_code_ = VXCORE_ERR_CANCELLED;
  } catch (const webdav::Failure &error) { execution_code_ = error.error; }
    catch (const std::bad_alloc &) { execution_code_ = VXCORE_ERR_OUT_OF_MEMORY; }
    catch (...) { execution_code_ = VXCORE_ERR_UNKNOWN; }
  try {
    backend_->SetCancellation(nullptr);
    cleanup_required_ = backend_->CandidateCleanupRequired();
    if (kind_ == "clone" && execution_code_ != VXCORE_OK)
      cleanup_required_ = !private_io::IsEmpty(PathFromUtf8(root_));
  } catch (...) {
    cleanup_required_ = true;
    if (execution_code_ == VXCORE_OK) execution_code_ = VXCORE_ERR_UNKNOWN;
  }
  phase_.store(Phase::kExecuted, std::memory_order_release);
  return execution_code_;
}

void SyncLifecycle::ValidateOwner(VxCoreContext &context) const {
  Require(&context == owner_ && context.sync_manager.get() == manager_, VXCORE_ERR_INVALID_STATE);
  {
    std::lock_guard<std::mutex> lock(manager_->state_mutex_);
    const auto lease = manager_->lifecycle_leases_.find(lease_key_);
    Require(lease != manager_->lifecycle_leases_.end() && lease->second == this,
            VXCORE_ERR_INVALID_STATE);
    const auto old = manager_->backends_.find(notebook_id_);
    Require((old == manager_->backends_.end() ? nullptr : old->second.get()) == previous_backend_,
            VXCORE_ERR_INVALID_STATE);
  }
  NoFollow(PathFromUtf8(root_));
  FileIdentity current;
  Check(GetFileIdentity(PathFromUtf8(root_), current, FileAccess::kPrivate));
  Require(root_identity_.SameObject(current), VXCORE_ERR_INVALID_STATE);
  if (kind_ == "clone") {
    RequirePrivateClone(context, root_);
  } else {
    auto *notebook = context.notebook_manager->GetNotebook(notebook_id_);
    Require(notebook == notebook_ && notebook && notebook->GetRootFolder() == root_,
            VXCORE_ERR_INVALID_STATE);
    Check(notebook->CheckWritable());
    Require(notebook->GetConfig().ToJson().dump() == owner_config_, VXCORE_ERR_INVALID_STATE);
    const auto config_path = PathFromUtf8(root_) / webdav::kConfigPath;
    NoFollow(config_path);
    Require(webdav::HashFile(config_path, nullptr, FileAccess::kPrivate) == config_sha256_ && BaselineHash(root_) == baseline_sha256_,
            VXCORE_ERR_INVALID_STATE);
  }
}

void SyncLifecycle::CompleteImpl(VxCoreContext &context) {
  Check(execution_code_);  // Preserve the actual execution failure over finalization errors.
  ValidateOwner(context);
  webdav::CheckCancelled(cancellation_);
  if (kind_ == "credentialRecovery") return;  // Proved cleanup, never publication.
  if (kind_ == "clone") {
    const auto config = webdav::NotebookJson(PathFromUtf8(root_) / webdav::kConfigPath, {}, FileAccess::kPrivate);
    notebook_id_ = Uuid(config.at("id"));
    Json facts;
    Check(InspectSyncStorage(root_, notebook_id_, facts));
    Require(facts.at("cleanCloneBaseline").get<bool>() &&
            facts.at("remoteUrl") == config_.remote_url, VXCORE_ERR_INVALID_STATE);
    std::string opened;
    Check(context.notebook_manager->OpenNotebook(root_, opened));
    // Registration is confined to this private context. Its owner must close
    // before a bridge-owned receipt can grant managed import authority.
    committed_ = true;
    Require(opened == notebook_id_, VXCORE_ERR_INVALID_STATE);
    return;
  }

  // Allocate nodes and map capacity before the account-binding commit. Insertion
  // then only moves already-owned nodes; no allocation follows durable publication.
  decltype(manager_->configs_cache_) configs;
  decltype(manager_->states_) states;
  decltype(manager_->backends_) backends;
  configs.emplace(notebook_id_, config_);
  states.emplace(notebook_id_, SyncState::kIdle);
  backends.emplace(notebook_id_, nullptr);
  auto config_node = configs.extract(notebook_id_);
  auto state_node = states.extract(notebook_id_);
  auto backend_node = backends.extract(notebook_id_);
  {
    std::lock_guard<std::mutex> lock(manager_->state_mutex_);
    const auto reserved = manager_->lifecycle_leases_.size();
    manager_->configs_cache_.reserve(manager_->configs_cache_.size() + reserved);
    manager_->states_.reserve(manager_->states_.size() + reserved);
    manager_->backends_.reserve(manager_->backends_.size() + reserved);
  }
  VxCoreError publication_code;
  try { publication_code = backend_->CommitCandidate(provider_); }
  catch (const webdav::Failure &error) { publication_code = error.error; }
  catch (const std::bad_alloc &) { publication_code = VXCORE_ERR_OUT_OF_MEMORY; }
  catch (...) { publication_code = VXCORE_ERR_UNKNOWN; }
  const bool published = backend_->CandidateCommitted();
  try { cleanup_required_ = backend_->CandidateCleanupRequired(); }
  catch (...) {
    cleanup_required_ = true;
    if (publication_code == VXCORE_OK) publication_code = VXCORE_ERR_UNKNOWN;
  }
  if (!published) {
    Check(publication_code);
    Require(false, VXCORE_ERR_INVALID_STATE);
  }
  // A reporting/cleanup failure cannot reverse the account already published
  // by the backend. Install the preallocated nodes before returning that error.
  backend_node.mapped() = std::move(backend_);
  std::unique_ptr<ISyncBackend> previous;
  {
    std::lock_guard<std::mutex> lock(manager_->state_mutex_);
    manager_->configs_cache_.erase(notebook_id_);
    manager_->states_.erase(notebook_id_);
    const auto old = manager_->backends_.find(notebook_id_);
    if (old != manager_->backends_.end()) previous = std::move(old->second);
    manager_->backends_.erase(notebook_id_);
    manager_->configs_cache_.insert(std::move(config_node));
    manager_->states_.insert(std::move(state_node));
    manager_->backends_.insert(std::move(backend_node));
  }
  committed_ = true;
  Check(publication_code);
}

nlohmann::json SyncLifecycle::Summary(VxCoreError code) const {
  return {{"version", 1}, {"operationId", operation_id_}, {"kind", kind_},
          {"notebookId", notebook_id_.empty() ? Json(nullptr) : Json(notebook_id_)},
          {"executed", executed_}, {"committed", committed_},
          {"cleanupRequired", cleanup_required_}, {"code", static_cast<int>(code)}};
}

VxCoreError SyncLifecycle::Complete(VxCoreContext &context, Json &out) noexcept {
  auto expected = Phase::kExecuted;
  if (&context != owner_ || !phase_.compare_exchange_strong(expected, Phase::kCompleting)) {
    // Reject an early owner call without reading worker-written result fields.
    // The acquired terminal phase is the only permission to read that snapshot.
    const auto phase = phase_.load(std::memory_order_acquire);
    try {
      if (phase == Phase::kExecuted || phase == Phase::kCompleted) {
        out = Summary(VXCORE_ERR_INVALID_STATE);
      } else {
        out = {{"version", 1}, {"operationId", operation_id_}, {"kind", kind_},
               {"notebookId", kind_ == "clone" ? Json(nullptr) : Json(lease_key_)},
               {"executed", false}, {"committed", false}, {"cleanupRequired", false},
               {"code", static_cast<int>(VXCORE_ERR_INVALID_STATE)}};
      }
    } catch (...) {}
    return VXCORE_ERR_INVALID_STATE;
  }
  try {
    CompleteImpl(context);
    completion_code_ = VXCORE_OK;
  } catch (const webdav::Failure &error) { completion_code_ = error.error; }
    catch (const std::bad_alloc &) { completion_code_ = VXCORE_ERR_OUT_OF_MEMORY; }
    catch (...) { completion_code_ = VXCORE_ERR_INVALID_STATE; }
  if (backend_) {
    try { cleanup_required_ = backend_->CandidateCleanupRequired() || cleanup_required_; }
    catch (...) { cleanup_required_ = true; }
  }
  phase_.store(Phase::kCompleted, std::memory_order_release);
  try { out = Summary(completion_code_); }
  catch (...) { return VXCORE_ERR_OUT_OF_MEMORY; }
  return completion_code_;
}

}  // namespace vxcore
