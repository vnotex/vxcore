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
// applied later via Initialize(); the provider is stored for the libgit2
// credential callback rewiring landing in Wave 6.3. Today SetCredentials()
// remains the active credential source.
GitSyncBackend::GitSyncBackend(const SyncConfig & /*cfg*/,
                               std::shared_ptr<ICredentialProvider> creds_provider)
    : GitSyncBackend() {
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
  credentials_ = {};
  initialized_ = false;
}

// Task 4.1 (sync-backend-phase4 F1.2): backend identity methods.
//
// GetName is the same lower-case ASCII key used in SyncConfig::backend / the
// `vx_notebook` JSON `syncBackend` field, so the upcoming SyncBackendRegistry
// (Task 4.2) can match instances to factory keys.
std::string GitSyncBackend::GetName() const { return "git"; }

// Capability bits for the git backend. Cancellation is deliberately OMITTED
// — Wave 12 of sync-backend-phase4 wires the cooperative cancellation token
// through libgit2 progress callbacks; until then, flipping the bit would
// lie to callers that ask "can I cancel a running sync?".
SyncCapabilities GitSyncBackend::GetCapabilities() const {
  return static_cast<uint32_t>(SyncCapability::ConflictDetection) |
         static_cast<uint32_t>(SyncCapability::ConflictResolution) |
         static_cast<uint32_t>(SyncCapability::IncrementalSync) |
         static_cast<uint32_t>(SyncCapability::AuthRequired) |
         static_cast<uint32_t>(SyncCapability::ProgressReporting);
}

// IsInitialized must reflect BOTH halves of the readiness contract:
//   1. libgit2 process-global init succeeded (LibGit2Init::ok()), AND
//   2. this backend's Initialize() drove one of the bootstrap branches to
//      VXCORE_OK (which sets initialized_ = true).
// Either half failing means subsequent Sync/Push/Pull calls would early-out
// with VXCORE_ERR_UNKNOWN; returning true would lie to callers.
bool GitSyncBackend::IsInitialized() const {
  return LibGit2Init::ok() && initialized_;
}

VxCoreError GitSyncBackend::SetCredentials(const SyncCredentials &creds) {
  std::lock_guard<std::mutex> lock(op_mutex_);
  credentials_ = creds;
  return VXCORE_OK;
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
  // Task 5.4 (F1.5): parse the opaque backend_options blob into a typed
  // view exactly once, so future tunables (ssl_verify, connect_timeout_ms,
  // proxy_url, ...) are addressed via options_.* instead of stringly-typed
  // json lookups scattered across the backend.
  options_ = GitOptions::FromJson(config_.backend_options);
  git_dir_ = root_folder_ + "/vx_notebook/vx_sync";

  // T17 corrupt-repo guard: vx_sync exists as a directory but has no HEAD —
  // the user has stray files or a broken libgit2 init under our state dir.
  // Reject loudly. We deliberately do NOT delete vx_sync (user data) and
  // require the user to inspect/clean it manually.
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

  // Branch 1 (T14): re-open an existing libgit2 repo at <git_dir_>.
  // We only treat the directory as a real repo if HEAD is present — bare
  // /vx_sync/ subdir without HEAD is treated as "no repo" and falls through
  // to NOT_IMPLEMENTED (T15/T16/T17 will handle clone/init/reject).
  if (IsDirectory(git_dir_) && IsRegularFile(head_path)) {
    VxCoreError err = OpenExistingRepo(root_folder_, git_dir_, config_,
                                       credentials_, &rebase_in_progress_,
                                       &repo_);
    if (err == VXCORE_OK) {
      initialized_ = true;
    }
    return err;
  }

  // T17 both-non-empty guard: vx_sync does NOT exist, the notebook root has
  // user files (anything other than vx_notebook/), AND the configured remote
  // already has refs. We can't safely choose between clone and init+push, so
  // we refuse and ask the user to empty one side.
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

    // Use a transient pipeline solely for the RemoteHasRefs probe — it only
    // touches config.remote_url + credentials, never repo_ (which is null
    // here anyway).
    GitSyncPipeline guard_pipeline(repo_, git_dir_, root_folder_, config_,
                                   credentials_, &rebase_in_progress_);
    if (has_user_files && guard_pipeline.RemoteHasRefs()) {
      VXCORE_LOG_ERROR(
          "GitSyncBackend::Initialize: both notebook root '%s' and remote '%s' "
          "are non-empty — refusing to proceed; empty one side and retry",
          root_folder_.c_str(), config_.remote_url.c_str());
      return VXCORE_ERR_INVALID_PARAM;
    }

    // T15: clone-into-empty branch. The notebook root has no user files
    // (only vx_notebook/ at most) and a remote URL is configured. We init a
    // libgit2 repo with the separate gitdir layout (vx_notebook/vx_sync/),
    // attach origin, fetch with credentials, then checkout the remote's
    // default branch (main, fall back to master). git_clone is intentionally
    // avoided — its Windows local-transport path hangs on file:// URLs (see
    // T10 learnings).
    if (!has_user_files && !config_.remote_url.empty()) {
      VxCoreError err = BootstrapFromEmptyRemote(
          root_folder_, git_dir_, config_, credentials_, &rebase_in_progress_,
          &repo_);
      if (err == VXCORE_OK) {
        initialized_ = true;
      }
      return err;
    }

    // T16: init+push branch. The notebook root has user files, the configured
    // remote is empty (RemoteHasRefs was false; T17 would have rejected
    // otherwise), and a remote URL is set. Initialize a libgit2 repo with the
    // separate-gitdir layout, write defaults, create origin, stage+commit
    // everything, and push refs/heads/main to publish the notebook.
    if (has_user_files && !config_.remote_url.empty()) {
      VxCoreError err = BootstrapToEmptyRemote(
          root_folder_, git_dir_, config_, credentials_, &rebase_in_progress_,
          &repo_);
      if (err == VXCORE_OK) {
        initialized_ = true;
      }
      return err;
    }
  }

  // No Initialize branch matched (e.g. has_user_files but no remote_url).
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

  GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
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

  // T25 retry loop: up to 3 attempts of fetch -> rebase -> push. Push can
  // race with a concurrent writer who pushed between our fetch and our push;
  // we re-fetch+rebase and retry with backoff. Conflicts short-circuit
  // immediately (the user must resolve before any further sync attempts).
  static const int kBackoffMs[3] = {100, 500, 2000};
  bool pushed = false;
  for (int attempt = 0; attempt < 3; ++attempt) {
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

    // Distinguish a non-fast-forward rejection (retry-worthy) from a hard
    // network/auth error (terminal). libgit2 surfaces non-FF rejections with
    // a textual message via git_error_last(); TranslateGitError already
    // collapsed to VXCORE_ERR_SYNC_NETWORK. Inspect the message we just
    // logged in TranslateGitError — but we don't have it here, so re-read.
    const git_error *gerr = git_error_last();
    bool non_ff = false;
    if (gerr && gerr->message) {
      const char *m = gerr->message;
      if (std::strstr(m, "non-fast-forward") != nullptr ||
          std::strstr(m, "non fast forward") != nullptr ||
          std::strstr(m, "fast-forward") != nullptr ||
          std::strstr(m, "rejected") != nullptr) {
        non_ff = true;
      }
    }
    if (!non_ff || attempt >= 2) {
      return err;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kBackoffMs[attempt]));
  }

  if (!pushed) {
    return VXCORE_ERR_SYNC_NETWORK;
  }

  ReportProgress(callback, userdata, SyncState::kIdle, "Sync complete", 1.0f);
  return VXCORE_OK;
}

// T20: map git_status_list to SyncFileInfo list. We honor the same status
// flags as `git status` and prefer the index_to_workdir path when available
// (so renames on the unstaged side surface the destination), falling back to
// head_to_index. Files outside our flag set (e.g. IGNORED, CURRENT) are
// silently skipped.
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
      // RENAMED / TYPECHANGE / IGNORED / CURRENT etc. - not in our enum.
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
  return std::unique_ptr<GitSyncPipeline>(
      new GitSyncPipeline(repo_, git_dir_, root_folder_, config_, credentials_,
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
  GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
                           &rebase_in_progress_);
  GitConflictResolver resolver(repo_, root_folder_);
  return resolver.ResolveConflict(path, resolution, pipeline);
}

// Task 4.3 (sync-backend-phase4 F1.1): self-registration of the git backend
// into SyncBackendRegistry. The token's constructor runs at static-init time
// and calls Registry::Register("git", factory). BackendRegistration swallows
// any exception so static-init can never crash the program.
namespace {
const BackendRegistration kGitRegistration{
    "git",
    [](const SyncConfig &cfg,
       std::shared_ptr<ICredentialProvider> provider) -> std::unique_ptr<ISyncBackend> {
      return std::make_unique<GitSyncBackend>(cfg, std::move(provider));
    }};
}  // namespace

// Anchor — touches the static registration token so MSVC /OPT:REF cannot
// dead-strip this translation unit once Task 4.4 removes SyncManager's
// direct std::make_unique<GitSyncBackend> reference. Called once from
// LibGit2Init::ctor via the g_git_backend_link_anchor function-pointer hook
// installed below at static-init time. The static_cast<void>(...) is a
// no-op load that the optimizer cannot prove is dead because
// kGitRegistration has a non-trivial ctor with side effects.
void EnsureGitBackendLinked() {
  static_cast<void>(&kGitRegistration);
}

// Cross-TU anchor hook installer — see libgit2_init.cpp for the matching
// std::atomic<void(*)()> declaration. Setting the pointer at static-init
// time means the first LibGit2Init ctor call (on any thread) will dispatch
// through it. When git_sync_backend.cpp is NOT linked into a test binary,
// the hook simply remains null and LibGit2Init's no-op branch is taken.
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
