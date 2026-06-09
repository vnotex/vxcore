#include "sync/git/git_repo_bootstrap.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "sync/credential_provider.h"
#include "sync/git/git_credential_callback.h"
#include "sync/git/git_defaults.h"
#include "sync/git/git_error_translator.h"
#include "sync/git/git_handles.h"
#include "sync/git/git_sync_pipeline.h"
#include "utils/file_utils.h"
#include "utils/logger.h"

namespace vxcore {

namespace {

// Shared helper for Branch 2 (clone) and Branch 3 (init+push).
//
// Wave 6.3 F4.4: credentials reference replaced with
// shared_ptr<ICredentialProvider>. The constructed transient pipeline holds
// its own shared_ptr copy; this helper does not need to retain the provider
// beyond returning.
VxCoreError InitEmptyRepoWithRemote(const std::string &root_folder,
                                    const std::string &git_dir,
                                    const SyncConfig &config,
                                    std::shared_ptr<ICredentialProvider> creds_provider,
                                    git_rebase **rebase_in_progress,
                                    const std::string &context,
                                    git_repository **out_repo) {
  *out_repo = nullptr;

  // Ensure parent directories exist before init_ext touches the gitdir.
  try {
    std::filesystem::create_directories(PathFromUtf8(git_dir));
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("%s: create_directories(%s) failed: %s", context.c_str(),
                     git_dir.c_str(), e.what());
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
  iopts.workdir_path = root_folder.c_str();
  iopts.initial_head = "main";

  git_repositoryPtr repo;
  {
    git_repository *raw = nullptr;
    rc = git_repository_init_ext(&raw, git_dir.c_str(), &iopts);
    repo.reset(raw);
  }
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  GitSyncPipeline pipeline(repo.get(), git_dir, root_folder, config, creds_provider,
                           rebase_in_progress);
  VxCoreError cfg_err = pipeline.ApplyDefaultGitConfig();
  if (cfg_err != VXCORE_OK) {
    return cfg_err;
  }

  WriteIfMissing(root_folder + "/.gitignore", BuildGitignoreContent(config));
  WriteIfMissing(root_folder + "/.gitattributes", kDefaultGitattributes);

  git_remotePtr remote;
  {
    git_remote *raw = nullptr;
    rc = git_remote_create(&raw, repo.get(), "origin", config.remote_url.c_str());
    remote.reset(raw);
  }
  if (rc != 0) {
    return TranslateGitError(rc);
  }
  remote.reset();

  *out_repo = repo.release();
  return VXCORE_OK;
}

}  // namespace

void ScrubGitlink(const std::string &root_folder, const std::string &context) {
  const std::string gitlink_path = root_folder + "/.git";
  if (PathExists(gitlink_path) && !IsDirectory(gitlink_path)) {
    std::error_code rm_ec;
    std::filesystem::remove(PathFromUtf8(gitlink_path), rm_ec);
    if (rm_ec) {
      VXCORE_LOG_WARN("%s: failed to remove gitlink %s: %s",
                      context.c_str(), gitlink_path.c_str(),
                      rm_ec.message().c_str());
    }
  }
}

VxCoreError OpenExistingRepo(const std::string &root_folder,
                             const std::string &git_dir,
                             const SyncConfig &config,
                             std::shared_ptr<ICredentialProvider> creds_provider,
                             git_rebase **rebase_in_progress,
                             git_repository **out_repo) {
  *out_repo = nullptr;

  git_repositoryPtr repo;
  int rc;
  {
    git_repository *raw = nullptr;
    rc = git_repository_open(&raw, git_dir.c_str());
    repo.reset(raw);
  }
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  rc = git_repository_set_workdir(repo.get(), root_folder.c_str(), /*update_gitlink=*/0);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  git_remotePtr remote;
  {
    git_remote *raw = nullptr;
    rc = git_remote_lookup(&raw, repo.get(), "origin");
    remote.reset(raw);
  }
  if (rc != 0) {
    VXCORE_LOG_ERROR(
        "GitSyncBackend::Initialize: existing repo at %s has no 'origin' remote",
        git_dir.c_str());
    return VXCORE_ERR_INVALID_PARAM;
  }

  const char *remote_url = git_remote_url(remote.get());
  if (remote_url == nullptr || config.remote_url != remote_url) {
    VXCORE_LOG_ERROR(
        "GitSyncBackend::Initialize: origin URL mismatch (existing='%s' config='%s')",
        remote_url ? remote_url : "(null)", config.remote_url.c_str());
    return VXCORE_ERR_INVALID_PARAM;
  }

  remote.reset();

  GitSyncPipeline pipeline(repo.get(), git_dir, root_folder, config, creds_provider,
                           rebase_in_progress);

  VxCoreError cfg_err = pipeline.ApplyDefaultGitConfig();
  if (cfg_err != VXCORE_OK) {
    return cfg_err;
  }

  *out_repo = repo.release();
  return VXCORE_OK;
}

VxCoreError BootstrapFromEmptyRemote(const std::string &root_folder,
                                     const std::string &git_dir,
                                     const SyncConfig &config,
                                     std::shared_ptr<ICredentialProvider> creds_provider,
                                     git_rebase **rebase_in_progress,
                                     git_repository **out_repo,
                                     SyncCancellation *cancellation) {
  *out_repo = nullptr;

  git_repositoryPtr repo;
  {
    git_repository *raw = nullptr;
    VxCoreError init_err = InitEmptyRepoWithRemote(
        root_folder, git_dir, config, creds_provider, rebase_in_progress,
        "Initialize(clone)", &raw);
    repo.reset(raw);
    if (init_err != VXCORE_OK) {
      return init_err;
    }
  }

  git_remotePtr remote;
  int rc;
  {
    git_remote *raw = nullptr;
    rc = git_remote_lookup(&raw, repo.get(), "origin");
    remote.reset(raw);
  }
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  git_fetch_options fopts = GIT_FETCH_OPTIONS_INIT;
  auto bundle = MakeRemoteCallbacks(creds_provider.get(), config.remote_url, cancellation);
  fopts.callbacks = bundle.callbacks;

  rc = git_remote_fetch(remote.get(), /*refspecs=*/nullptr, &fopts,
                        "vnote initial fetch");
  remote.reset();
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  git_referencePtr origin_ref;
  {
    git_reference *raw = nullptr;
    rc = git_reference_lookup(&raw, repo.get(), "refs/remotes/origin/main");
    origin_ref.reset(raw);
  }
  if (rc != 0) {
    git_error_clear();
    git_reference *raw = nullptr;
    rc = git_reference_lookup(&raw, repo.get(),
                              "refs/remotes/origin/master");
    origin_ref.reset(raw);
  }
  if (rc != 0) {
    git_error_clear();
    *out_repo = repo.release();
    return VXCORE_OK;
  }

  git_objectPtr target;
  {
    git_object *raw = nullptr;
    rc = git_reference_peel(&raw, origin_ref.get(), GIT_OBJECT_COMMIT);
    target.reset(raw);
  }
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  git_checkout_options copts = GIT_CHECKOUT_OPTIONS_INIT;
  copts.checkout_strategy = GIT_CHECKOUT_SAFE;
  rc = git_checkout_tree(repo.get(), target.get(), &copts);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  git_oid target_oid = *git_object_id(target.get());
  target.reset();
  origin_ref.reset();

  git_referencePtr local_ref;
  {
    git_reference *raw = nullptr;
    rc = git_reference_create(&raw, repo.get(), "refs/heads/main",
                              &target_oid, /*force=*/0,
                              "vnote initial checkout");
    local_ref.reset(raw);
  }
  if (rc != 0) {
    return TranslateGitError(rc);
  }
  local_ref.reset();

  ScrubGitlink(root_folder, "Initialize(clone)");

  *out_repo = repo.release();
  return VXCORE_OK;
}

VxCoreError BootstrapToEmptyRemote(const std::string &root_folder,
                                   const std::string &git_dir,
                                   const SyncConfig &config,
                                   std::shared_ptr<ICredentialProvider> creds_provider,
                                   git_rebase **rebase_in_progress,
                                   git_repository **out_repo) {
  *out_repo = nullptr;

  git_repositoryPtr repo;
  {
    git_repository *raw = nullptr;
    VxCoreError init_err = InitEmptyRepoWithRemote(
        root_folder, git_dir, config, creds_provider, rebase_in_progress,
        "Initialize(initpush)", &raw);
    repo.reset(raw);
    if (init_err != VXCORE_OK) {
      return init_err;
    }
  }

  GitSyncPipeline pipeline(repo.get(), git_dir, root_folder, config, creds_provider,
                           rebase_in_progress);

  VxCoreError err = pipeline.StageAll();
  if (err != VXCORE_OK) {
    return err;
  }
  err = pipeline.CommitIndex("VNote sync: initial commit");
  if (err != VXCORE_OK) {
    return err;
  }
  err = pipeline.PushOrigin();
  if (err != VXCORE_OK) {
    return err;
  }

  ScrubGitlink(root_folder, "Initialize(initpush)");

  *out_repo = repo.release();
  return VXCORE_OK;
}

}  // namespace vxcore
