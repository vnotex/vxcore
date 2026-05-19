#include "sync/git_sync_backend.h"

#include <git2.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "sync/git/git_config_fixer.h"
#include "sync/git/git_credential_callback.h"
#include "sync/git/git_defaults.h"
#include "sync/git/git_error_translator.h"
#include "sync/git/git_sync_pipeline.h"
#include "sync/git/gitkeep_sweeper.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/string_utils.h"
#include "utils/utils.h"

namespace vxcore {

namespace {

// T28: TranslateGitError moved to sync/git/git_error_translator.{h,cpp}.

}  // namespace

GitSyncBackend::GitSyncBackend() = default;

GitSyncBackend::~GitSyncBackend() {
  // Best-effort shutdown to release libgit2 handles.
  Shutdown();
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

  // Pre-open fix moved to sync/git/git_config_fixer.{h,cpp}.
  RebindCoreWorktree(root_folder);

  // Branch 1 (T14): re-open an existing libgit2 repo at <git_dir_>.
  // We only treat the directory as a real repo if HEAD is present — bare
  // /vx_sync/ subdir without HEAD is treated as "no repo" and falls through
  // to NOT_IMPLEMENTED (T15/T16/T17 will handle clone/init/reject).
  if (IsDirectory(git_dir_) && IsRegularFile(head_path)) {
    git_repository *repo = nullptr;
    int rc = git_repository_open(&repo, git_dir_.c_str());
    if (rc != 0) {
      // Corrupt or unreadable repo — surface the libgit2 error class via
      // TranslateGitError, but do NOT auto-delete vx_sync (user data).
      return TranslateGitError(rc);
    }

    // Bind the working tree to the notebook root.
    rc = git_repository_set_workdir(repo, root_folder_.c_str(), /*update_gitlink=*/0);
    if (rc != 0) {
      VxCoreError err = TranslateGitError(rc);
      git_repository_free(repo);
      return err;
    }

    // Look up the "origin" remote.
    git_remote *remote = nullptr;
    rc = git_remote_lookup(&remote, repo, "origin");
    if (rc != 0) {
      // Missing origin: T14 contract says reject with INVALID_PARAM.
      VXCORE_LOG_ERROR(
          "GitSyncBackend::Initialize: existing repo at %s has no 'origin' remote",
          git_dir_.c_str());
      git_repository_free(repo);
      return VXCORE_ERR_INVALID_PARAM;
    }

    const char *remote_url = git_remote_url(remote);
    if (remote_url == nullptr || config_.remote_url != remote_url) {
      VXCORE_LOG_ERROR(
          "GitSyncBackend::Initialize: origin URL mismatch (existing='%s' config='%s')",
          remote_url ? remote_url : "(null)", config_.remote_url.c_str());
      git_remote_free(remote);
      git_repository_free(repo);
      return VXCORE_ERR_INVALID_PARAM;
    }

    git_remote_free(remote);
    repo_ = repo;

    GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
                             &rebase_in_progress_);

    // T18: enforce baseline git config (filemode, autocrlf, gpgsign, author).
    VxCoreError cfg_err = pipeline.ApplyDefaultGitConfig();
    if (cfg_err != VXCORE_OK) {
      git_repository_free(repo_);
      repo_ = nullptr;
      return cfg_err;
    }

    initialized_ = true;
    return VXCORE_OK;
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

    GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
                             &rebase_in_progress_);
    if (has_user_files && pipeline.RemoteHasRefs()) {
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
      // Ensure parent directories exist before init_ext touches the gitdir.
      try {
        std::filesystem::create_directories(PathFromUtf8(git_dir_));
      } catch (const std::exception &e) {
        VXCORE_LOG_ERROR("Initialize(clone): create_directories(%s) failed: %s",
                         git_dir_.c_str(), e.what());
        return VXCORE_ERR_UNKNOWN;
      }

      git_repository_init_options iopts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
      int rc = git_repository_init_options_init(
          &iopts, GIT_REPOSITORY_INIT_OPTIONS_VERSION);
      if (rc != 0) {
        return TranslateGitError(rc);
      }
      // NO_DOTGIT_DIR keeps the gitdir literally at <git_dir_> instead of
      // nesting ".git" beneath it; matches the spike T1 layout that
      // GitSyncBackend re-opens in Branch 1.
      iopts.flags = GIT_REPOSITORY_INIT_NO_REINIT |
                    GIT_REPOSITORY_INIT_MKDIR |
                    GIT_REPOSITORY_INIT_MKPATH |
                    GIT_REPOSITORY_INIT_NO_DOTGIT_DIR;
      iopts.workdir_path = root_folder_.c_str();
      iopts.initial_head = "main";

      rc = git_repository_init_ext(&repo_, git_dir_.c_str(), &iopts);
      if (rc != 0) {
        VxCoreError err = TranslateGitError(rc);
        repo_ = nullptr;
        return err;
      }

      GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
                               &rebase_in_progress_);
      VxCoreError cfg_err = pipeline.ApplyDefaultGitConfig();
      if (cfg_err != VXCORE_OK) {
        git_repository_free(repo_);
        repo_ = nullptr;
        return cfg_err;
      }

      // Defaults are best-effort; failure to write doesn't abort clone.
      WriteIfMissing(root_folder_ + "/.gitignore", BuildGitignoreContent(config_));
      WriteIfMissing(root_folder_ + "/.gitattributes", kDefaultGitattributes);

      git_remote *remote = nullptr;
      rc = git_remote_create(&remote, repo_, "origin",
                             config_.remote_url.c_str());
      if (rc != 0) {
        VxCoreError err = TranslateGitError(rc);
        git_repository_free(repo_);
        repo_ = nullptr;
        return err;
      }

      git_fetch_options fopts = GIT_FETCH_OPTIONS_INIT;
      GitCredentialPayload pl{credentials_.personal_access_token};
      fopts.callbacks = MakeRemoteCallbacks(&pl);

      rc = git_remote_fetch(remote, /*refspecs=*/nullptr, &fopts,
                            "vnote initial fetch");
      git_remote_free(remote);
      remote = nullptr;
      if (rc != 0) {
        VxCoreError err = TranslateGitError(rc);
        git_repository_free(repo_);
        repo_ = nullptr;
        return err;
      }

      // Resolve the remote default branch. Prefer main, fall back to master.
      // If neither exists, the remote is truly empty (no commits yet) — leave
      // the local repo as-is with HEAD symbolically pointing at refs/heads/main
      // and let the future Sync() create the first commit.
      git_reference *origin_ref = nullptr;
      rc = git_reference_lookup(&origin_ref, repo_,
                                "refs/remotes/origin/main");
      if (rc != 0) {
        git_error_clear();
        rc = git_reference_lookup(&origin_ref, repo_,
                                  "refs/remotes/origin/master");
      }
      if (rc != 0) {
        git_error_clear();
        initialized_ = true;
        return VXCORE_OK;
      }

      git_object *target = nullptr;
      rc = git_reference_peel(&target, origin_ref, GIT_OBJECT_COMMIT);
      if (rc != 0) {
        VxCoreError err = TranslateGitError(rc);
        git_reference_free(origin_ref);
        git_repository_free(repo_);
        repo_ = nullptr;
        return err;
      }

      git_checkout_options copts = GIT_CHECKOUT_OPTIONS_INIT;
      copts.checkout_strategy = GIT_CHECKOUT_SAFE;
      rc = git_checkout_tree(repo_, target, &copts);
      if (rc != 0) {
        VxCoreError err = TranslateGitError(rc);
        git_object_free(target);
        git_reference_free(origin_ref);
        git_repository_free(repo_);
        repo_ = nullptr;
        return err;
      }

      // Snapshot the OID before freeing the peeled object, then create the
      // local refs/heads/main pointing at it. HEAD was set to a symbolic
      // refs/heads/main by init_ext; this binding makes it resolve.
      git_oid target_oid = *git_object_id(target);
      git_object_free(target);
      git_reference_free(origin_ref);

      git_reference *local_ref = nullptr;
      rc = git_reference_create(&local_ref, repo_, "refs/heads/main",
                                &target_oid, /*force=*/0,
                                "vnote initial checkout");
      if (rc != 0) {
        VxCoreError err = TranslateGitError(rc);
        git_repository_free(repo_);
        repo_ = nullptr;
        return err;
      }
      git_reference_free(local_ref);

      // libgit2 writes a `.git` gitlink file in the workdir when the gitdir
      // lives elsewhere (so plain `git` CLI can find it). VNote doesn't want
      // notebooks to look like git working trees to external tools, so we
      // remove the gitlink — vx_notebook/vx_sync/ remains the canonical
      // gitdir and GitSyncBackend re-open uses git_repository_open on it
      // directly. Only delete a regular file (the gitlink), never a real
      // user `.git` directory.
      const std::string gitlink_path = root_folder_ + "/.git";
      if (PathExists(gitlink_path) && !IsDirectory(gitlink_path)) {
        std::error_code rm_ec;
        std::filesystem::remove(PathFromUtf8(gitlink_path), rm_ec);
        if (rm_ec) {
          VXCORE_LOG_WARN("Initialize(clone): failed to remove gitlink %s: %s",
                          gitlink_path.c_str(), rm_ec.message().c_str());
        }
      }

      initialized_ = true;
      return VXCORE_OK;
    }

    // T16: init+push branch. The notebook root has user files, the configured
    // remote is empty (RemoteHasRefs was false; T17 would have rejected
    // otherwise), and a remote URL is set. Initialize a libgit2 repo with the
    // separate-gitdir layout, write defaults, create origin, stage+commit
    // everything, and push refs/heads/main to publish the notebook.
    if (has_user_files && !config_.remote_url.empty()) {
      try {
        std::filesystem::create_directories(PathFromUtf8(git_dir_));
      } catch (const std::exception &e) {
        VXCORE_LOG_ERROR("Initialize(initpush): create_directories(%s) failed: %s",
                         git_dir_.c_str(), e.what());
        return VXCORE_ERR_UNKNOWN;
      }

      git_repository_init_options iopts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
      int rc = git_repository_init_options_init(
          &iopts, GIT_REPOSITORY_INIT_OPTIONS_VERSION);
      if (rc != 0) {
        return TranslateGitError(rc);
      }
      iopts.flags = GIT_REPOSITORY_INIT_NO_REINIT |
                    GIT_REPOSITORY_INIT_MKDIR |
                    GIT_REPOSITORY_INIT_MKPATH |
                    GIT_REPOSITORY_INIT_NO_DOTGIT_DIR;
      iopts.workdir_path = root_folder_.c_str();
      iopts.initial_head = "main";

      rc = git_repository_init_ext(&repo_, git_dir_.c_str(), &iopts);
      if (rc != 0) {
        VxCoreError err = TranslateGitError(rc);
        repo_ = nullptr;
        return err;
      }

      GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
                               &rebase_in_progress_);
      VxCoreError cfg_err = pipeline.ApplyDefaultGitConfig();
      if (cfg_err != VXCORE_OK) {
        git_repository_free(repo_);
        repo_ = nullptr;
        return cfg_err;
      }

      // Defaults are best-effort; failure to write doesn't abort init+push.
      WriteIfMissing(root_folder_ + "/.gitignore", BuildGitignoreContent(config_));
      WriteIfMissing(root_folder_ + "/.gitattributes", kDefaultGitattributes);

      git_remote *remote = nullptr;
      rc = git_remote_create(&remote, repo_, "origin",
                             config_.remote_url.c_str());
      if (rc != 0) {
        VxCoreError err = TranslateGitError(rc);
        git_repository_free(repo_);
        repo_ = nullptr;
        return err;
      }
      git_remote_free(remote);

      // Stage everything currently on disk (user files + just-written
      // .gitignore/.gitattributes), commit as the initial root commit, then
      // push refs/heads/main so the empty remote becomes the source of truth.
      // Helpers assume op_mutex_ already held — we hold it for the whole
      // Initialize so don't re-lock.
      VxCoreError err = pipeline.StageAll();
      if (err != VXCORE_OK) {
        git_repository_free(repo_);
        repo_ = nullptr;
        return err;
      }
      err = pipeline.CommitIndex("VNote sync: initial commit");
      if (err != VXCORE_OK) {
        git_repository_free(repo_);
        repo_ = nullptr;
        return err;
      }
      err = pipeline.PushOrigin();
      if (err != VXCORE_OK) {
        git_repository_free(repo_);
        repo_ = nullptr;
        return err;
      }

      // Same gitlink scrub as T15: libgit2 leaves a .git gitlink file in the
      // workdir pointing at our separate gitdir. VNote re-opens via the
      // gitdir directly, so we don't want notebooks to look like git working
      // trees to external tools. Only delete a regular file (the gitlink),
      // never a real user `.git` directory.
      const std::string gitlink_path = root_folder_ + "/.git";
      if (PathExists(gitlink_path) && !IsDirectory(gitlink_path)) {
        std::error_code rm_ec;
        std::filesystem::remove(PathFromUtf8(gitlink_path), rm_ec);
        if (rm_ec) {
          VXCORE_LOG_WARN("Initialize(initpush): failed to remove gitlink %s: %s",
                          gitlink_path.c_str(), rm_ec.message().c_str());
        }
      }

      initialized_ = true;
      return VXCORE_OK;
    }
  }

  // No Initialize branch matched (e.g. has_user_files but no remote_url).
  return VXCORE_ERR_NOT_IMPLEMENTED;
}

VxCoreError GitSyncBackend::Shutdown() {
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
  return VXCORE_OK;
}

// T6: gitkeep sweep moved to sync/git/gitkeep_sweeper.{h,cpp}.

VxCoreError GitSyncBackend::Sync(SyncProgressCallback callback, void *userdata) {
  std::unique_lock<std::mutex> lock(op_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return VXCORE_ERR_SYNC_IN_PROGRESS;
  }
  if (!initialized_) {
    return VXCORE_ERR_UNKNOWN;
  }

  // T26: build SyncProgress and dispatch to user callback. Inline lambda
  // keeps the noisy nullable-callback guard out of every phase site.
  auto emit = [&](SyncState state, const char *msg, float pct) {
    if (!callback) {
      return;
    }
    SyncProgress progress;
    progress.message = msg;
    progress.percentage = pct;
    progress.current_state = state;
    callback(progress, userdata);
  };

  GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
                           &rebase_in_progress_);

  emit(SyncState::kStaging, "Staging", 0.10f);
  EnsureGitkeepFiles(root_folder_, config_);
  VxCoreError err = pipeline.StageAll();
  if (err != VXCORE_OK) {
    return err;
  }

  emit(SyncState::kStaging, "Committing", 0.20f);
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
    emit(SyncState::kFetching, "Fetching", 0.40f);
    VXCORE_LOG_DEBUG("GitSyncBackend::Sync: step=fetch starting");
    err = pipeline.FetchOrigin();
    VXCORE_LOG_DEBUG("GitSyncBackend::Sync: step=fetch rc=%d", err);
    if (err != VXCORE_OK) {
      return err;
    }

    emit(SyncState::kAnalyzing, "Analyzing", 0.50f);

    emit(SyncState::kMerging, "Rebasing", 0.70f);
    VXCORE_LOG_DEBUG("GitSyncBackend::Sync: step=rebase starting");
    err = pipeline.RebaseOntoOrigin();
    VXCORE_LOG_DEBUG("GitSyncBackend::Sync: step=rebase rc=%d", err);
    if (err == VXCORE_ERR_SYNC_CONFLICT) {
      return VXCORE_ERR_SYNC_CONFLICT;
    }
    if (err != VXCORE_OK) {
      return err;
    }

    emit(SyncState::kPushing, "Pushing", 0.90f);
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

  emit(SyncState::kIdle, "Sync complete", 1.0f);
  return VXCORE_OK;
}

VxCoreError GitSyncBackend::Push(SyncProgressCallback callback, void *userdata) {
  std::unique_lock<std::mutex> lock(op_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return VXCORE_ERR_SYNC_IN_PROGRESS;
  }
  if (!initialized_) {
    return VXCORE_ERR_UNKNOWN;
  }

  auto emit = [&](SyncState state, const char *msg, float pct) {
    if (!callback) return;
    SyncProgress progress;
    progress.message = msg;
    progress.percentage = pct;
    progress.current_state = state;
    callback(progress, userdata);
  };

  GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
                           &rebase_in_progress_);

  emit(SyncState::kStaging, "Staging", 0.20f);
  EnsureGitkeepFiles(root_folder_, config_);
  VxCoreError err = pipeline.StageAll();
  if (err != VXCORE_OK) {
    return err;
  }

  emit(SyncState::kStaging, "Committing", 0.40f);
  err = pipeline.CommitIndex("VNote sync push");
  if (err != VXCORE_OK) {
    return err;
  }

  emit(SyncState::kPushing, "Pushing", 0.80f);
  err = pipeline.PushOrigin();
  if (err != VXCORE_OK) {
    return err;
  }

  emit(SyncState::kIdle, "Push complete", 1.0f);
  return VXCORE_OK;
}

VxCoreError GitSyncBackend::Pull(SyncProgressCallback callback, void *userdata) {
  std::unique_lock<std::mutex> lock(op_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return VXCORE_ERR_SYNC_IN_PROGRESS;
  }
  if (!initialized_) {
    return VXCORE_ERR_UNKNOWN;
  }

  auto emit = [&](SyncState state, const char *msg, float pct) {
    if (!callback) return;
    SyncProgress progress;
    progress.message = msg;
    progress.percentage = pct;
    progress.current_state = state;
    callback(progress, userdata);
  };

  GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
                           &rebase_in_progress_);

  // CRITICAL: auto-commit any uncommitted local changes BEFORE fetching so
  // the upcoming rebase has a clean working tree to replay onto. Without
  // this, a Pull on a notebook with edits in flight would either lose them
  // or refuse to rebase.
  emit(SyncState::kStaging, "Staging", 0.10f);
  VxCoreError err = pipeline.StageAll();
  if (err != VXCORE_OK) {
    return err;
  }
  emit(SyncState::kStaging, "Committing", 0.20f);
  err = pipeline.CommitIndex("VNote sync auto-commit before pull");
  if (err != VXCORE_OK) {
    return err;
  }

  emit(SyncState::kFetching, "Fetching", 0.50f);
  err = pipeline.FetchOrigin();
  if (err != VXCORE_OK) {
    return err;
  }

  emit(SyncState::kMerging, "Rebasing", 0.80f);
  err = pipeline.RebaseOntoOrigin();
  if (err == VXCORE_ERR_SYNC_CONFLICT) {
    return VXCORE_ERR_SYNC_CONFLICT;
  }
  if (err != VXCORE_OK) {
    return err;
  }

  emit(SyncState::kIdle, "Pull complete", 1.0f);
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

  git_status_list *list = nullptr;
  int rc = git_status_list_new(&list, repo_, &opts);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  const size_t n = git_status_list_entrycount(list);
  for (size_t i = 0; i < n; ++i) {
    const git_status_entry *entry = git_status_byindex(list, i);
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
      // RENAMED / TYPECHANGE / IGNORED / CURRENT etc. — not in our enum.
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

  git_status_list_free(list);
  return VXCORE_OK;
}

VxCoreError GitSyncBackend::StageAllForTesting() {
  std::lock_guard<std::mutex> lock(op_mutex_);
  if (!initialized_) {
    return VXCORE_ERR_UNKNOWN;
  }
  GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
                           &rebase_in_progress_);
  return pipeline.StageAll();
}

VxCoreError GitSyncBackend::CommitIndexForTesting(const std::string &message) {
  std::lock_guard<std::mutex> lock(op_mutex_);
  if (!initialized_) {
    return VXCORE_ERR_UNKNOWN;
  }
  GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
                           &rebase_in_progress_);
  return pipeline.CommitIndex(message);
}

VxCoreError GitSyncBackend::FetchOriginForTesting() {
  std::lock_guard<std::mutex> lock(op_mutex_);
  if (!initialized_) {
    return VXCORE_ERR_UNKNOWN;
  }
  GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
                           &rebase_in_progress_);
  return pipeline.FetchOrigin();
}

VxCoreError GitSyncBackend::RebaseOntoOriginForTesting() {
  std::lock_guard<std::mutex> lock(op_mutex_);
  if (!initialized_) {
    return VXCORE_ERR_UNKNOWN;
  }
  GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
                           &rebase_in_progress_);
  return pipeline.RebaseOntoOrigin();
}

VxCoreError GitSyncBackend::PushOriginForTesting() {
  std::lock_guard<std::mutex> lock(op_mutex_);
  if (!initialized_) {
    return VXCORE_ERR_UNKNOWN;
  }
  GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
                           &rebase_in_progress_);
  return pipeline.PushOrigin();
}

VxCoreError GitSyncBackend::GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) {
  std::lock_guard<std::mutex> lock(op_mutex_);
  if (!initialized_) {
    return VXCORE_ERR_UNKNOWN;
  }

  out_conflicts.clear();

  git_index *idx = nullptr;
  int rc = git_repository_index(&idx, repo_);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  git_index_conflict_iterator *iter = nullptr;
  rc = git_index_conflict_iterator_new(&iter, idx);
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_index_free(idx);
    return err;
  }

  const auto now_unix_secs = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  for (;;) {
    const git_index_entry *ancestor = nullptr;
    const git_index_entry *our = nullptr;
    const git_index_entry *their = nullptr;
    rc = git_index_conflict_next(&ancestor, &our, &their, iter);
    if (rc == GIT_ITEROVER) {
      git_error_clear();
      break;
    }
    if (rc != 0) {
      VxCoreError err = TranslateGitError(rc);
      git_index_conflict_iterator_free(iter);
      git_index_free(idx);
      return err;
    }

    const char *path = nullptr;
    if (our && our->path) path = our->path;
    else if (their && their->path) path = their->path;
    else if (ancestor && ancestor->path) path = ancestor->path;
    if (path == nullptr) {
      continue;
    }

    SyncConflictInfo info;
    info.path = path;

    // local_modified_utc: file mtime in workdir as unix seconds, 0 on error.
    info.local_modified_utc = 0;
    try {
      const std::string abs = root_folder_ + "/" + info.path;
      if (PathExists(abs)) {
        auto ftime = std::filesystem::last_write_time(PathFromUtf8(abs));
        // Convert file_time_type -> system_clock unix seconds via clock offset.
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now());
        info.local_modified_utc = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                sctp.time_since_epoch())
                .count());
      }
    } catch (const std::exception &e) {
      VXCORE_LOG_WARN("GetConflicts: mtime(%s) failed: %s", info.path.c_str(), e.what());
      info.local_modified_utc = 0;
    }

    // remote_modified_utc: approximate as "now" — git stores no per-file
    // remote mtime, and the conflict was just observed during fetch+rebase.
    info.remote_modified_utc = now_unix_secs;

    // is_binary: query libgit2's blob heuristic on the THEIR side (if any).
    info.is_binary = false;
    if (their != nullptr) {
      git_blob *blob = nullptr;
      int brc = git_blob_lookup(&blob, repo_, &their->id);
      if (brc == 0 && blob != nullptr) {
        info.is_binary = (git_blob_is_binary(blob) != 0);
        git_blob_free(blob);
      } else {
        git_error_clear();
      }
    }

    out_conflicts.push_back(std::move(info));
  }

  git_index_conflict_iterator_free(iter);
  git_index_free(idx);
  return VXCORE_OK;
}

VxCoreError GitSyncBackend::ResolveConflict(const std::string &path,
                                            SyncConflictResolution resolution) {
  std::lock_guard<std::mutex> lock(op_mutex_);
  if (!initialized_) {
    return VXCORE_ERR_UNKNOWN;
  }
  if (repo_ == nullptr) {
    return VXCORE_ERR_UNKNOWN;
  }

  git_index *idx = nullptr;
  int rc = git_repository_index(&idx, repo_);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  // Look up the conflict triple for this path. KeepLocal doesn't strictly
  // need it (just clears + stages), but every branch needs to know the
  // conflict actually exists, so we look it up once.
  const git_index_entry *ancestor = nullptr;
  const git_index_entry *our = nullptr;
  const git_index_entry *their = nullptr;
  rc = git_index_conflict_get(&ancestor, &our, &their, idx, path.c_str());
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_index_free(idx);
    return err;
  }

  const std::string abs_original = root_folder_ + "/" + path;

  // Rebase semantic flip: when libgit2 rebases LOCAL onto REMOTE, the index
  // conflict triple labels REMOTE as "our" (the rebase target / upstream)
  // and LOCAL as "their" (the commit being replayed). Map them back to the
  // user-facing names so the resolution does what the user asked.
  const git_index_entry *remote_side = our;
  const git_index_entry *local_side = their;

  // Helper to write a blob's raw content into an absolute workdir path.
  // Returns VXCORE_OK on success or when |entry| is null (caller decides
  // semantics for null-side cases, e.g. delete-on-remote).
  auto write_blob_to_path = [&](const git_index_entry *entry,
                                const std::string &abs) -> VxCoreError {
    if (entry == nullptr) {
      return VXCORE_OK;
    }
    git_blob *blob = nullptr;
    int brc = git_blob_lookup(&blob, repo_, &entry->id);
    if (brc != 0 || blob == nullptr) {
      return TranslateGitError(brc);
    }
    const void *raw = git_blob_rawcontent(blob);
    git_object_size_t raw_size = git_blob_rawsize(blob);
    try {
      std::filesystem::create_directories(PathFromUtf8(abs).parent_path());
    } catch (...) {
    }
    std::ofstream ofs(PathFromUtf8(abs), std::ios::binary | std::ios::trunc);
    if (!ofs) {
      git_blob_free(blob);
      return VXCORE_ERR_UNKNOWN;
    }
    if (raw != nullptr && raw_size > 0) {
      ofs.write(static_cast<const char *>(raw),
                static_cast<std::streamsize>(raw_size));
    }
    git_blob_free(blob);
    return VXCORE_OK;
  };

  switch (resolution) {
    case SyncConflictResolution::kKeepBoth: {
      // Build conflict filename: <dir>/<stem>.sync-conflict-<ts><ext>.
      std::string dir, base;
      auto slash = path.find_last_of('/');
      if (slash == std::string::npos) {
        dir.clear();
        base = path;
      } else {
        dir = path.substr(0, slash + 1);
        base = path.substr(slash + 1);
      }
      std::string stem, ext;
      auto dot = base.find_last_of('.');
      if (dot == std::string::npos || dot == 0) {
        stem = base;
        ext.clear();
      } else {
        stem = base.substr(0, dot);
        ext = base.substr(dot);
      }
      const int64_t ts = GetCurrentTimestampMillis() / 1000;
      const std::string conflict_rel =
          dir + stem + ".sync-conflict-" + std::to_string(ts) + ext;
      const std::string abs_conflict = root_folder_ + "/" + conflict_rel;

      // Write LOCAL blob (rebase "their") to the .sync-conflict file.
      VxCoreError werr = write_blob_to_path(local_side, abs_conflict);
      if (werr != VXCORE_OK) {
        git_index_free(idx);
        return werr;
      }
      // Overwrite original with REMOTE blob (rebase "our").
      werr = write_blob_to_path(remote_side, abs_original);
      if (werr != VXCORE_OK) {
        git_index_free(idx);
        return werr;
      }

      // Stage both the original (now REMOTE content) and the conflict file.
      int rc2 = git_index_add_bypath(idx, path.c_str());
      if (rc2 != 0) {
        VxCoreError err = TranslateGitError(rc2);
        git_index_free(idx);
        return err;
      }
      rc2 = git_index_add_bypath(idx, conflict_rel.c_str());
      if (rc2 != 0) {
        VxCoreError err = TranslateGitError(rc2);
        git_index_free(idx);
        return err;
      }
      rc2 = git_index_conflict_remove(idx, path.c_str());
      if (rc2 != 0) {
        VxCoreError err = TranslateGitError(rc2);
        git_index_free(idx);
        return err;
      }
      rc2 = git_index_write(idx);
      git_index_free(idx);
      idx = nullptr;
      if (rc2 != 0) {
        return TranslateGitError(rc2);
      }
      break;
    }

    case SyncConflictResolution::kKeepLocal: {
      // Materialize LOCAL blob (rebase "their") to workdir so the file
      // doesn't retain any conflict markers libgit2 may have written.
      if (local_side != nullptr) {
        VxCoreError werr = write_blob_to_path(local_side, abs_original);
        if (werr != VXCORE_OK) {
          git_index_free(idx);
          return werr;
        }
      } else {
        // No LOCAL side (deleted locally): drop the workdir file.
        std::error_code ec;
        std::filesystem::remove(PathFromUtf8(abs_original), ec);
      }

      int rc2 = git_index_add_bypath(idx, path.c_str());
      if (rc2 != 0) {
        git_error_clear();
        rc2 = git_index_remove_bypath(idx, path.c_str());
        if (rc2 != 0) {
          VxCoreError err = TranslateGitError(rc2);
          git_index_free(idx);
          return err;
        }
      }
      int rc3 = git_index_conflict_remove(idx, path.c_str());
      if (rc3 != 0) {
        VxCoreError err = TranslateGitError(rc3);
        git_index_free(idx);
        return err;
      }
      rc3 = git_index_write(idx);
      git_index_free(idx);
      idx = nullptr;
      if (rc3 != 0) {
        return TranslateGitError(rc3);
      }
      break;
    }

    case SyncConflictResolution::kKeepRemote: {
      // Overwrite workdir with REMOTE blob (rebase "our"), stage, clear.
      if (remote_side != nullptr) {
        VxCoreError werr = write_blob_to_path(remote_side, abs_original);
        if (werr != VXCORE_OK) {
          git_index_free(idx);
          return werr;
        }
      } else {
        // No REMOTE side (deleted on remote): drop the workdir file.
        std::error_code ec;
        std::filesystem::remove(PathFromUtf8(abs_original), ec);
      }

      int rc2 = git_index_add_bypath(idx, path.c_str());
      if (rc2 != 0) {
        git_error_clear();
        rc2 = git_index_remove_bypath(idx, path.c_str());
        if (rc2 != 0) {
          VxCoreError err = TranslateGitError(rc2);
          git_index_free(idx);
          return err;
        }
      }
      int rc3 = git_index_conflict_remove(idx, path.c_str());
      if (rc3 != 0) {
        VxCoreError err = TranslateGitError(rc3);
        git_index_free(idx);
        return err;
      }
      rc3 = git_index_write(idx);
      git_index_free(idx);
      idx = nullptr;
      if (rc3 != 0) {
        return TranslateGitError(rc3);
      }
      break;
    }
  }

  GitSyncPipeline pipeline(repo_, git_dir_, root_folder_, config_, credentials_,
                           &rebase_in_progress_);
  return pipeline.ContinueRebaseAfterResolution();
}

int GitSyncBackend::WriteDefaultIgnoreAndAttributesForTesting(const std::string &dir,
                                                               const SyncConfig &config) {
  int written = 0;
  if (WriteIfMissing(dir + "/.gitignore", BuildGitignoreContent(config))) ++written;
  if (WriteIfMissing(dir + "/.gitattributes", kDefaultGitattributes)) ++written;
  return written;
}

VxCoreError GitSyncBackend::TranslateGitErrorForTesting(int git_rc) {
  return vxcore::TranslateGitError(git_rc);
}

}  // namespace vxcore
