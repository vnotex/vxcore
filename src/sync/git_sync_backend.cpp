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
  (void)root_folder;
  (void)config;
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
