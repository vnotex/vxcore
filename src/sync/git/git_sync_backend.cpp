#include "sync/git/git_sync_backend.h"

#include "sync/sync_backend_registry.h"

#include <git2.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>
#include <vector>

#include "sync/git/git_config_fixer.h"
#include "sync/git/git_conflict_resolver.h"
#include "sync/git/git_defaults.h"
#include "sync/git/git_error_translator.h"
#include "sync/git/git_handles.h"
#include "sync/git/git_repo_bootstrap.h"
#include "sync/git/git_sync_pipeline.h"
#include "sync/git/gitkeep_sweeper.h"
#include "sync/git/libgit2_init.h"
#include "utils/file_utils.h"
#include "utils/logger.h"

namespace vxcore {

// Helper to report progress via callback. Handles null callback gracefully.
static void ReportProgress(const SyncProgressCallback &callback, void *userdata,
                           SyncState state, std::string_view message, float percentage) {
  if (!callback) {
    return;
  }
  SyncProgress progress;
  progress.current_state = state;
  progress.message = std::string(message);
  progress.percentage = percentage;
  callback(progress, userdata);
}

GitSyncBackend::GitSyncBackend() = default;

// SyncConfig + ICredentialProvider ctor required by the SyncBackendRegistry
// factory signature (Task 6.2 F4.4 of sync-backend-phase4). Real config is
// applied later via Initialize(); the provider is stored under
// creds_provider_mu_ and is the SOLE credential source going forward.
// Wave 6.3 (F4.4) removed the legacy SetCredentials path entirely.
GitSyncBackend::GitSyncBackend(const SyncConfig & /*cfg*/,
                               std::shared_ptr<ICredentialProvider> creds_provider)
    : GitSyncBackend() {
  std::lock_guard<std::mutex> lock(creds_provider_mu_);
  creds_provider_ = std::move(creds_provider);
}

GitSyncBackend::~GitSyncBackend() {
  // Best-effort teardown — inline the former Shutdown() body. Wave 4.5
  // (F1.3 of sync-backend-phase4) removed the standalone Shutdown() virtual;
  // teardown now happens only through destruction. Hold op_mutex_ to serialize
  // against any concurrent in-flight call (defensive; in practice destruction
  // races are the caller's responsibility).
  std::lock_guard<std::mutex> lock(op_mutex_);
  if (rebase_in_progress_ != nullptr) {
    git_rebase_free(rebase_in_progress_);
    rebase_in_progress_ = nullptr;
  }
  if (repo_ != nullptr) {
    git_repository_free(repo_);
    repo_ = nullptr;
  }
  initialized_ = false;
}

std::string GitSyncBackend::GetName() const { return "git"; }

SyncCapabilities GitSyncBackend::GetCapabilities() const {
  return static_cast<uint32_t>(SyncCapability::ConflictDetection) |
         static_cast<uint32_t>(SyncCapability::ConflictResolution) |
         static_cast<uint32_t>(SyncCapability::IncrementalSync) |
         static_cast<uint32_t>(SyncCapability::AuthRequired) |
         static_cast<uint32_t>(SyncCapability::ProgressReporting);
}

bool GitSyncBackend::IsInitialized() const {
  return LibGit2Init::ok() && initialized_;
}

// Wave 6.3 F4.4 of sync-backend-phase4: atomic credential-provider swap.
// Holds creds_provider_mu_ ONLY for the shared_ptr replacement; never for
// any libgit2 operation. The libgit2 credential callback never touches
// this mutex — payloads are stack-allocated snapshots built by
// MakeRemoteCallbacks at each network phase, before any libgit2 thread
// enters the picture.
void GitSyncBackend::ReplaceCredsProvider(
    std::shared_ptr<ICredentialProvider> provider) {
  std::lock_guard<std::mutex> lock(creds_provider_mu_);
  creds_provider_ = std::move(provider);
}

// Returns the current provider as a fresh shared_ptr copy. Used by Sync(),
// Initialize(), ResolveConflict(), and MakePipeline() to capture a per-call
// snapshot that survives even if ReplaceCredsProvider fires mid-operation.
std::shared_ptr<ICredentialProvider> GitSyncBackend::GetCredsProviderSnapshot() const {
  std::lock_guard<std::mutex> lock(creds_provider_mu_);
  return creds_provider_;
}

VxCoreError GitSyncBackend::Initialize(const std::string &root_folder,
                                       const SyncConfig &config) {
  std::lock_guard<std::mutex> lock(op_mutex_);

  // Idempotent: a second Initialize on an already-initialized backend is a
  // no-op. Re-validating against config_ would risk rejecting a benign repeat
  // call (e.g. SyncManager re-enabling), so we just return OK.
  if (initialized_) {
    return VXCORE_OK;
  }

  root_folder_ = root_folder;
  config_ = config;
  // Task 5.4 (F1.5): parse the opaque backend_options blob into a typed view.
  options_ = GitOptions::FromJson(config_.backend_options);
  git_dir_ = root_folder_ + "/vx_notebook/vx_sync";

  // Snapshot provider once for the whole Initialize pass.
  std::shared_ptr<ICredentialProvider> provider_snapshot;
  {
    std::lock_guard<std::mutex> p_lock(creds_provider_mu_);
    provider_snapshot = creds_provider_;
  }

  // T17 corrupt-repo guard: vx_sync exists as a directory but has no HEAD —
  // refuse loudly. We deliberately do NOT delete vx_sync (user data).
  const std::string head_path = git_dir_ + "/HEAD";
  if (IsDirectory(git_dir_) && !IsRegularFile(head_path)) {
    VXCORE_LOG_ERROR(
        "GitSyncBackend::Initialize: corrupt or non-repo state at %s "
        "(directory exists but HEAD missing) — refusing to proceed; "
        "inspect/remove the directory manually",
        git_dir_.c_str());
    return VXCORE_ERR_UNKNOWN;
  }

  RebindCoreWorktree(root_folder);

  // Branch 1 (T14): re-open existing repo.
  if (IsDirectory(git_dir_) && IsRegularFile(head_path)) {
    VxCoreError err = OpenExistingRepo(root_folder_, git_dir_, config_,
                                       provider_snapshot, &rebase_in_progress_,
                                       &repo_);
    if (err == VXCORE_OK) {
      initialized_ = true;
    }
    return err;
  }

  // T17 both-non-empty guard.
  if (!PathExists(git_dir_)) {
    bool has_user_files = false;
    try {
      for (auto &entry :
           std::filesystem::directory_iterator(PathFromUtf8(root_folder_))) {
        const std::string name = PathToUtf8(entry.path().filename());
        if (name == "vx_notebook") {
          continue;
        }
        has_user_files = true;
        break;
      }
    } catch (const std::exception &e) {
      VXCORE_LOG_ERROR("GitSyncBackend::Initialize: directory_iterator(%s) failed: %s",
                       root_folder_.c_str(), e.what());
    }

    // Transient pipeline solely for the RemoteHasRefs probe.
    GitSyncPipeline guard_pipeline(repo_, git_dir_, root_folder_, config_,
                                   provider_snapshot, &rebase_in_progress_);
    if (has_user_files && guard_pipeline.RemoteHasRefs()) {
      VXCORE_LOG_ERROR(
          "GitSyncBackend::Initialize: both notebook root '%s' and remote '%s' "
          "are non-empty — refusing to proceed; empty one side and retry",
          root_folder_.c_str(), config_.remote_url.c_str());
      return VXCORE_ERR_INVALID_PARAM;
    }

    // T15: clone-into-empty branch.
    if (!has_user_files && !config_.remote_url.empty()) {
      VxCoreError err = BootstrapFromEmptyRemote(
          root_folder_, git_dir_, config_, provider_snapshot, &rebase_in_progress_,
          &repo_);
      if (err == VXCORE_OK) {
        initialized_ = true;
      }
      return err;
    }

    // T16: init+push branch.
    if (has_user_files && !config_.remote_url.empty()) {
      VxCoreError err = BootstrapToEmptyRemote(
          root_folder_, git_dir_, config_, provider_snapshot, &rebase_in_progress_,
          &repo_);
      if (err == VXCORE_OK) {
        initialized_ = true;
      }
      return err;
    }
  }

  return VXCORE_ERR_NOT_IMPLEMENTED;
}

VxCoreError GitSyncBackend::Sync(SyncProgressCallback callback, void *userdata) {
  std::unique_lock<std::mutex> lock(op_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return VXCORE_ERR_SYNC_IN_PROGRESS;
  }
  if (!initialized_) {
    return VXCORE_ERR_UNKNOWN;
  }

  // Wave 6.3 F4.4: snapshot provider at Sync() entry — atomic per-call.
  // A concurrent ReplaceCredsProvider() takes effect on the NEXT Sync() only.
  std::shared_ptr<ICredentialProvider> provider_snapshot;
  {
    std::lock_guard<std::mutex> p_lock(creds_provider_mu_);
    provider_snapshot = creds_provider_;
  }

  GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, provider_snapshot,
                           &rebase_in_progress_);

  ReportProgress(callback, userdata, SyncState::kStaging, "Staging", 0.10f);
  EnsureGitkeepFiles(root_folder_, config_);
  VxCoreError err = pipeline.StageAll();
  if (err != VXCORE_OK) {
    return err;
  }

  ReportProgress(callback, userdata, SyncState::kStaging, "Committing", 0.20f);
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  VXCORE_LOG_DEBUG("GitSyncBackend::Sync: step=commit starting");
  err = pipeline.CommitIndex(std::string("VNote sync ") + std::to_string(now_ms));
  VXCORE_LOG_DEBUG("GitSyncBackend::Sync: step=commit rc=%d", err);
  if (err != VXCORE_OK) {
    return err;
  }

  // F5.10 / Task 11.1 of sync-backend-phase4: retry loop driven by
  // RetryPolicy + compute_delay. Network errors (per is_retryable_network_error)
  // AND non-fast-forward push rejections (libgit2 surfaces these as generic
  // GIT_ERROR with klass GIT_ERROR_REFERENCE — NOT in the network whitelist,
  // so detected separately by message inspection here) trigger a backoff +
  // retry. Every other failure returns immediately to the caller.
  bool pushed = false;
  for (int attempt = 1; attempt <= retry_policy_.max_attempts; ++attempt) {
    if (attempt > 1) {
      auto delay = compute_delay(attempt, retry_policy_, retry_rng_);
      VXCORE_LOG_DEBUG("GitSyncBackend::Sync: backoff before attempt=%d delay_ms=%lld",
                       attempt, static_cast<long long>(delay.count()));
      std::this_thread::sleep_for(delay);
    }
    VXCORE_LOG_DEBUG("GitSyncBackend::Sync: step=push attempt=%d", attempt);
    ReportProgress(callback, userdata, SyncState::kFetching, "Fetching", 0.40f);
    VXCORE_LOG_DEBUG("GitSyncBackend::Sync: step=fetch starting");
    err = pipeline.FetchOrigin();
    VXCORE_LOG_DEBUG("GitSyncBackend::Sync: step=fetch rc=%d", err);
    if (err != VXCORE_OK) {
      return err;
    }

    ReportProgress(callback, userdata, SyncState::kAnalyzing, "Analyzing", 0.50f);

    ReportProgress(callback, userdata, SyncState::kMerging, "Rebasing", 0.70f);
    VXCORE_LOG_DEBUG("GitSyncBackend::Sync: step=rebase starting");
    err = pipeline.RebaseOntoOrigin();
    VXCORE_LOG_DEBUG("GitSyncBackend::Sync: step=rebase rc=%d", err);
    if (err == VXCORE_ERR_SYNC_CONFLICT) {
      return VXCORE_ERR_SYNC_CONFLICT;
    }
    if (err != VXCORE_OK) {
      return err;
    }

    ReportProgress(callback, userdata, SyncState::kPushing, "Pushing", 0.90f);
    VXCORE_LOG_DEBUG("GitSyncBackend::Sync: step=push starting");
    err = pipeline.PushOrigin();
    VXCORE_LOG_DEBUG("GitSyncBackend::Sync: step=push rc=%d", err);
    if (err == VXCORE_OK) {
      pushed = true;
      break;
    }

    // W11.2 (B3): Classify failure via structured GitOpError instead of
    // ad-hoc strstr() against libgit2's English message. The translator is
    // the single owner of message-text inspection. Capture the message
    // BEFORE any subsequent libgit2 call wipes git_error_last().
    const git_error *gerr = git_error_last();
    const std::string gerr_msg =
        (gerr && gerr->message) ? std::string(gerr->message) : std::string();
    const GitOpError op_err = ClassifyGitOp(-1, gerr_msg);
    const bool non_ff = (op_err == GitOpError::NonFastForward);
    const bool retryable =
        non_ff || (err == VXCORE_ERR_SYNC_NETWORK);
    if (!retryable || attempt >= retry_policy_.max_attempts) {
      return err;
    }
    // Loop continues; next iteration's compute_delay() sleep applies.
  }

  if (!pushed) {
    return VXCORE_ERR_SYNC_NETWORK;
  }

  ReportProgress(callback, userdata, SyncState::kIdle, "Sync complete", 1.0f);
  return VXCORE_OK;
}

VxCoreError GitSyncBackend::GetStatus(std::vector<SyncFileInfo> &out_files) {
  std::lock_guard<std::mutex> lock(op_mutex_);
  if (!initialized_) {
    return VXCORE_ERR_UNKNOWN;
  }

  out_files.clear();

  git_status_options opts = GIT_STATUS_OPTIONS_INIT;
  opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
               GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
               GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR;

  git_status_listPtr list;
  git_status_list *raw_list = nullptr;
  int rc = git_status_list_new(&raw_list, repo_, &opts);
  if (rc != 0) {
    return TranslateGitError(rc);
  }
  list.reset(raw_list);

  const size_t n = git_status_list_entrycount(list.get());
  for (size_t i = 0; i < n; ++i) {
    const git_status_entry *entry = git_status_byindex(list.get(), i);
    if (entry == nullptr) {
      continue;
    }

    const unsigned int s = entry->status;
    SyncFileStatus mapped = SyncFileStatus::kUnchanged;
    if ((s & GIT_STATUS_CONFLICTED) != 0) {
      mapped = SyncFileStatus::kConflicted;
    } else if ((s & (GIT_STATUS_INDEX_NEW | GIT_STATUS_WT_NEW)) != 0) {
      mapped = SyncFileStatus::kAddedLocal;
    } else if ((s & (GIT_STATUS_INDEX_MODIFIED | GIT_STATUS_WT_MODIFIED)) != 0) {
      mapped = SyncFileStatus::kModifiedLocal;
    } else if ((s & (GIT_STATUS_INDEX_DELETED | GIT_STATUS_WT_DELETED)) != 0) {
      mapped = SyncFileStatus::kDeletedLocal;
    } else {
      continue;
    }

    const char *path = nullptr;
    if (entry->index_to_workdir != nullptr &&
        entry->index_to_workdir->new_file.path != nullptr) {
      path = entry->index_to_workdir->new_file.path;
    } else if (entry->head_to_index != nullptr &&
               entry->head_to_index->new_file.path != nullptr) {
      path = entry->head_to_index->new_file.path;
    }
    if (path == nullptr) {
      continue;
    }

    SyncFileInfo info;
    info.path = path;
    info.status = mapped;
    out_files.push_back(std::move(info));
  }

  return VXCORE_OK;
}

std::unique_ptr<GitSyncPipeline> GitSyncBackend::MakePipeline() {
  std::lock_guard<std::mutex> lock(op_mutex_);
  if (!initialized_) {
    return nullptr;
  }
  std::shared_ptr<ICredentialProvider> provider_snapshot;
  {
    std::lock_guard<std::mutex> p_lock(creds_provider_mu_);
    provider_snapshot = creds_provider_;
  }
  return std::unique_ptr<GitSyncPipeline>(
      new GitSyncPipeline(repo_, git_dir_, root_folder_, config_, provider_snapshot,
                          &rebase_in_progress_));
}

VxCoreError GitSyncBackend::GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) {
  std::lock_guard<std::mutex> lock(op_mutex_);
  if (!initialized_) {
    return VXCORE_ERR_UNKNOWN;
  }
  GitConflictResolver resolver(repo_, root_folder_);
  return resolver.GetConflicts(out_conflicts);
}

VxCoreError GitSyncBackend::ResolveConflict(const std::string &path,
                                            SyncConflictResolution resolution) {
  std::lock_guard<std::mutex> lock(op_mutex_);
  if (!initialized_) {
    return VXCORE_ERR_UNKNOWN;
  }
  std::shared_ptr<ICredentialProvider> provider_snapshot;
  {
    std::lock_guard<std::mutex> p_lock(creds_provider_mu_);
    provider_snapshot = creds_provider_;
  }
  GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, provider_snapshot,
                           &rebase_in_progress_);
  GitConflictResolver resolver(repo_, root_folder_);
  return resolver.ResolveConflict(path, resolution, pipeline);
}

// Task 4.3 (sync-backend-phase4 F1.1): self-registration of the git backend.
namespace {
const BackendRegistration kGitRegistration{
    "git",
    [](const SyncConfig &cfg,
       std::shared_ptr<ICredentialProvider> provider) -> std::unique_ptr<ISyncBackend> {
      return std::make_unique<GitSyncBackend>(cfg, std::move(provider));
    }};
}  // namespace

void EnsureGitBackendLinked() {
  static_cast<void>(&kGitRegistration);
}

extern std::atomic<void (*)()> g_git_backend_link_anchor;
namespace {
struct GitBackendAnchorInstaller {
  GitBackendAnchorInstaller() noexcept {
    g_git_backend_link_anchor.store(&EnsureGitBackendLinked);
  }
};
const GitBackendAnchorInstaller kGitBackendAnchorInstaller;
}  // namespace

}  // namespace vxcore
