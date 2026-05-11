#include "sync/git_sync_backend.h"

#include <git2.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "utils/file_utils.h"
#include "utils/logger.h"

namespace vxcore {

namespace {

// Default .gitignore for VNote sync repos. Excludes the libgit2 gitdir
// (vx_notebook/vx_sync/) and common transient/OS files. User edits to the
// on-disk .gitignore are preserved (WriteIfMissing skips existing files).
constexpr const char *kDefaultGitignore =
    "# VNote sync defaults — feel free to edit; this file is preserved on update.\n"
    "vx_notebook/vx_sync/\n"
    "*.vswp\n"
    "*.tmp\n"
    ".DS_Store\n"
    "Thumbs.db\n";

// Default .gitattributes: normalize text line endings, mark common binary types
// so libgit2 / git skips text merges on them.
constexpr const char *kDefaultGitattributes =
    "# VNote sync defaults — feel free to edit; this file is preserved on update.\n"
    "* text=auto eol=lf\n"
    "*.png binary\n"
    "*.jpg binary\n"
    "*.jpeg binary\n"
    "*.gif binary\n"
    "*.pdf binary\n"
    "*.zip binary\n"
    "*.mp4 binary\n";

// Write |content| to |abs_path| only if the file does not already exist.
// Returns true if the file was created, false if it already existed or on error.
// UTF-8 safe on Windows: uses PathExists / PathFromUtf8.
bool WriteIfMissing(const std::string &abs_path, const std::string &content) {
  if (PathExists(abs_path)) {
    return false;
  }
  try {
    std::ofstream ofs(PathFromUtf8(abs_path), std::ios::binary | std::ios::trunc);
    if (!ofs) {
      VXCORE_LOG_ERROR("WriteIfMissing: failed to open %s for writing", abs_path.c_str());
      return false;
    }
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!ofs) {
      VXCORE_LOG_ERROR("WriteIfMissing: failed to write %s", abs_path.c_str());
      return false;
    }
    return true;
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("WriteIfMissing(%s) exception: %s", abs_path.c_str(), e.what());
    return false;
  }
}

// Build the full .gitignore content for a given config: defaults plus any
// user-configured extra excludes from SyncConfig::exclude_paths.
std::string BuildGitignoreContent(const SyncConfig &config) {
  std::string out(kDefaultGitignore);
  if (!config.exclude_paths.empty()) {
    out += "\n# From SyncConfig::exclude_paths\n";
    for (const auto &p : config.exclude_paths) {
      out += p;
      out += '\n';
    }
  }
  return out;
}

// T28: translate libgit2 return codes / error classes to VxCoreError. Always
// reads git_error_last() (and logs it) so the original libgit2 message is
// preserved in the log even when we collapse many klass values into a single
// VxCoreError. libgit2 v1.7.x has no GIT_ERROR_TCP / GIT_ERROR_NOTFOUND klass
// constants; we rely on the return-code constants (GIT_ENOTFOUND,
// GIT_EINVALIDSPEC) for those cases instead.
VxCoreError TranslateGitError(int git_rc) {
  if (git_rc >= 0) {
    return VXCORE_OK;
  }

  const git_error *err = git_error_last();
  const char *message = (err && err->message) ? err->message : "(no message)";
  int klass = err ? err->klass : 0;
  VXCORE_LOG_ERROR("libgit2 error rc=%d klass=%d: %s", git_rc, klass, message);

  if (git_rc == GIT_EAUTH) {
    return VXCORE_ERR_SYNC_AUTH_FAILED;
  }

  if (klass == GIT_ERROR_HTTP) {
    if (message != nullptr &&
        (std::strstr(message, "401") != nullptr ||
         std::strstr(message, "authenticat") != nullptr ||
         std::strstr(message, "credentials") != nullptr)) {
      return VXCORE_ERR_SYNC_AUTH_FAILED;
    }
    return VXCORE_ERR_SYNC_NETWORK;
  }

  if (klass == GIT_ERROR_NET || klass == GIT_ERROR_SSL || klass == GIT_ERROR_SSH) {
    return VXCORE_ERR_SYNC_NETWORK;
  }

  if (klass == GIT_ERROR_MERGE || klass == GIT_ERROR_CHECKOUT) {
    return VXCORE_ERR_SYNC_CONFLICT;
  }

  if (git_rc == GIT_ENOTFOUND) {
    return VXCORE_ERR_NOT_FOUND;
  }

  if (klass == GIT_ERROR_INVALID || git_rc == GIT_EINVALIDSPEC) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  return VXCORE_ERR_UNKNOWN;
}

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

    // T18: enforce baseline git config (filemode, autocrlf, gpgsign, author).
    VxCoreError cfg_err = ApplyDefaultGitConfig();
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

    if (has_user_files && RemoteHasRefs()) {
      VXCORE_LOG_ERROR(
          "GitSyncBackend::Initialize: both notebook root '%s' and remote '%s' "
          "are non-empty — refusing to proceed; empty one side and retry",
          root_folder_.c_str(), config_.remote_url.c_str());
      return VXCORE_ERR_INVALID_PARAM;
    }
  }

  // Other Initialize branches (T15: clone, T16: init+push) are
  // not implemented yet.
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

// T18: open the repo-level config and force the four invariants. Caller holds
// op_mutex_ and has repo_ open. Returns the first failing translation, or
// VXCORE_OK on full success. Always frees the cfg handle.
VxCoreError GitSyncBackend::ApplyDefaultGitConfig() {
  if (repo_ == nullptr) {
    return VXCORE_ERR_UNKNOWN;
  }
  git_config *cfg = nullptr;
  int rc = git_repository_config(&cfg, repo_);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  const char *user_name = credentials_.author_name.empty()
                              ? "VNote Sync"
                              : credentials_.author_name.c_str();
  const char *user_email = credentials_.author_email.empty()
                               ? "sync@vnote.local"
                               : credentials_.author_email.c_str();

  // core.autocrlf is set as a string ("false") to match git's textual config
  // representation; the others are booleans.
  rc = git_config_set_bool(cfg, "core.filemode", 0);
  if (rc == 0) rc = git_config_set_string(cfg, "core.autocrlf", "false");
  if (rc == 0) rc = git_config_set_bool(cfg, "commit.gpgsign", 0);
  if (rc == 0) rc = git_config_set_string(cfg, "user.name", user_name);
  if (rc == 0) rc = git_config_set_string(cfg, "user.email", user_email);

  git_config_free(cfg);
  if (rc != 0) {
    return TranslateGitError(rc);
  }
  return VXCORE_OK;
}

// T17 helper: build a detached anonymous remote pointing at config_.remote_url
// (no repo needed), connect for fetch (with our credential callback), and
// ls-remote. Returns true iff at least one ref is advertised. On any libgit2
// failure we log and return false — the caller treats unreachable remotes as
// "no refs" so the both-non-empty guard does not falsely reject network
// hiccups.
bool GitSyncBackend::RemoteHasRefs() {
  if (config_.remote_url.empty()) {
    return false;
  }

  git_remote *remote = nullptr;
  int rc = git_remote_create_detached(&remote, config_.remote_url.c_str());
  if (rc != 0) {
    const git_error *err = git_error_last();
    VXCORE_LOG_ERROR("RemoteHasRefs: git_remote_create_detached failed rc=%d: %s",
                     rc, (err && err->message) ? err->message : "(no message)");
    return false;
  }

  git_remote_callbacks cb = GIT_REMOTE_CALLBACKS_INIT;
  cb.credentials = &GitSyncBackendCredentialCb;
  cb.payload = this;

  rc = git_remote_connect(remote, GIT_DIRECTION_FETCH, &cb,
                          /*proxy_opts=*/nullptr, /*custom_headers=*/nullptr);
  if (rc != 0) {
    const git_error *err = git_error_last();
    VXCORE_LOG_ERROR("RemoteHasRefs: git_remote_connect failed rc=%d: %s",
                     rc, (err && err->message) ? err->message : "(no message)");
    git_remote_free(remote);
    return false;
  }

  const git_remote_head **heads = nullptr;
  size_t n_heads = 0;
  rc = git_remote_ls(&heads, &n_heads, remote);
  bool has_refs = false;
  if (rc == 0) {
    has_refs = (n_heads > 0);
  } else {
    const git_error *err = git_error_last();
    VXCORE_LOG_ERROR("RemoteHasRefs: git_remote_ls failed rc=%d: %s",
                     rc, (err && err->message) ? err->message : "(no message)");
  }

  git_remote_disconnect(remote);
  git_remote_free(remote);
  return has_refs;
}

VxCoreError GitSyncBackend::Sync(SyncProgressCallback callback, void *userdata) {
  (void)callback; (void)userdata;
  return VXCORE_ERR_NOT_IMPLEMENTED;
}

VxCoreError GitSyncBackend::Push(SyncProgressCallback callback, void *userdata) {
  (void)callback; (void)userdata;
  return VXCORE_ERR_NOT_IMPLEMENTED;
}

VxCoreError GitSyncBackend::Pull(SyncProgressCallback callback, void *userdata) {
  (void)callback; (void)userdata;
  return VXCORE_ERR_NOT_IMPLEMENTED;
}

VxCoreError GitSyncBackend::GetStatus(std::vector<SyncFileInfo> &out_files) {
  (void)out_files;
  return VXCORE_ERR_NOT_IMPLEMENTED;
}

VxCoreError GitSyncBackend::GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) {
  (void)out_conflicts;
  return VXCORE_ERR_NOT_IMPLEMENTED;
}

VxCoreError GitSyncBackend::ResolveConflict(const std::string &path,
                                            SyncConflictResolution resolution) {
  (void)path; (void)resolution;
  return VXCORE_ERR_NOT_IMPLEMENTED;
}

int GitSyncBackend::WriteDefaultIgnoreAndAttributesForTesting(const std::string &dir,
                                                              const SyncConfig &config) {
  int written = 0;
  if (WriteIfMissing(dir + "/.gitignore", BuildGitignoreContent(config))) ++written;
  if (WriteIfMissing(dir + "/.gitattributes", kDefaultGitattributes)) ++written;
  return written;
}

VxCoreError GitSyncBackend::TranslateGitErrorForTesting(int git_rc) {
  return TranslateGitError(git_rc);
}

// T27: libgit2 credential callback. v1 only honors PAT (HTTPS) and anonymous;
// the legacy SyncCredentials::username/password fields are intentionally
// ignored here (forbidden by plan guardrails). Returning GIT_PASSTHROUGH lets
// libgit2 try the next credential type or fail with an auth error which T28
// then translates.
int GitSyncBackendCredentialCb(git_credential **out, const char *url,
                               const char *username_from_url,
                               unsigned int allowed_types, void *payload) {
  (void)url;
  (void)username_from_url;
  auto *self = static_cast<GitSyncBackend *>(payload);
  if (self == nullptr) {
    return GIT_PASSTHROUGH;
  }
  if (!self->credentials_.personal_access_token.empty() &&
      (allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT) != 0) {
    return git_credential_userpass_plaintext_new(
        out, "x-access-token", self->credentials_.personal_access_token.c_str());
  }
  return GIT_PASSTHROUGH;
}

}  // namespace vxcore
