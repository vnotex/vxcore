#include "sync/git/git_repo_bootstrap.h"

#include <filesystem>
#include <string>
#include <system_error>

#include "sync/git/git_credential_callback.h"
#include "sync/git/git_defaults.h"
#include "sync/git/git_error_translator.h"
#include "sync/git/git_sync_pipeline.h"
#include "utils/file_utils.h"
#include "utils/logger.h"

namespace vxcore {

namespace {

// Shared helper for Branch 2 (clone) and Branch 3 (init+push).
//
// Performs the textually-identical preamble both bootstrap branches share:
//   1. create_directories(git_dir)
//   2. git_repository_init_options_init + NO_DOTGIT_DIR + MKDIR + MKPATH +
//      NO_REINIT, workdir_path = root_folder, initial_head = "main"
//   3. git_repository_init_ext(&repo, git_dir, &iopts)
//   4. GitSyncPipeline::ApplyDefaultGitConfig() — enforces filemode,
//      autocrlf, gpgsign, author
//   5. WriteIfMissing(.gitignore, BuildGitignoreContent(config))
//   6. WriteIfMissing(.gitattributes, kDefaultGitattributes)
//   7. git_remote_create(repo, "origin", config.remote_url)
//      — frees the remote handle immediately after creation; callers that
//      need it (Branch 2's fetch) do git_remote_lookup themselves.
//
// |context| is one of "Initialize(clone)" / "Initialize(initpush)" and is
// embedded in log messages so error output is byte-equivalent to the
// previously-inlined branches.
//
// On success: *out_repo holds the opened repo with origin remote created.
// On failure: *out_repo is nullptr; any partially-created handles freed.
VxCoreError InitEmptyRepoWithRemote(const std::string &root_folder,
                                    const std::string &git_dir,
                                    const SyncConfig &config,
                                    const SyncCredentials &credentials,
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
  // NO_DOTGIT_DIR keeps the gitdir literally at <git_dir> instead of nesting
  // ".git" beneath it; matches the spike T1 layout that GitSyncBackend
  // re-opens in Branch 1.
  iopts.flags = GIT_REPOSITORY_INIT_NO_REINIT |
                GIT_REPOSITORY_INIT_MKDIR |
                GIT_REPOSITORY_INIT_MKPATH |
                GIT_REPOSITORY_INIT_NO_DOTGIT_DIR;
  iopts.workdir_path = root_folder.c_str();
  iopts.initial_head = "main";

  git_repository *repo = nullptr;
  rc = git_repository_init_ext(&repo, git_dir.c_str(), &iopts);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  GitSyncPipeline pipeline(repo, git_dir, root_folder, config, credentials,
                           rebase_in_progress);
  VxCoreError cfg_err = pipeline.ApplyDefaultGitConfig();
  if (cfg_err != VXCORE_OK) {
    git_repository_free(repo);
    return cfg_err;
  }

  // Defaults are best-effort; failure to write doesn't abort init.
  WriteIfMissing(root_folder + "/.gitignore", BuildGitignoreContent(config));
  WriteIfMissing(root_folder + "/.gitattributes", kDefaultGitattributes);

  git_remote *remote = nullptr;
  rc = git_remote_create(&remote, repo, "origin", config.remote_url.c_str());
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_repository_free(repo);
    return err;
  }
  git_remote_free(remote);

  *out_repo = repo;
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
                             const SyncCredentials &credentials,
                             git_rebase **rebase_in_progress,
                             git_repository **out_repo) {
  *out_repo = nullptr;

  git_repository *repo = nullptr;
  int rc = git_repository_open(&repo, git_dir.c_str());
  if (rc != 0) {
    // Corrupt or unreadable repo — surface the libgit2 error class via
    // TranslateGitError, but do NOT auto-delete vx_sync (user data).
    return TranslateGitError(rc);
  }

  // Bind the working tree to the notebook root.
  rc = git_repository_set_workdir(repo, root_folder.c_str(), /*update_gitlink=*/0);
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
        git_dir.c_str());
    git_repository_free(repo);
    return VXCORE_ERR_INVALID_PARAM;
  }

  const char *remote_url = git_remote_url(remote);
  if (remote_url == nullptr || config.remote_url != remote_url) {
    VXCORE_LOG_ERROR(
        "GitSyncBackend::Initialize: origin URL mismatch (existing='%s' config='%s')",
        remote_url ? remote_url : "(null)", config.remote_url.c_str());
    git_remote_free(remote);
    git_repository_free(repo);
    return VXCORE_ERR_INVALID_PARAM;
  }

  git_remote_free(remote);

  GitSyncPipeline pipeline(repo, git_dir, root_folder, config, credentials,
                           rebase_in_progress);

  // T18: enforce baseline git config (filemode, autocrlf, gpgsign, author).
  VxCoreError cfg_err = pipeline.ApplyDefaultGitConfig();
  if (cfg_err != VXCORE_OK) {
    git_repository_free(repo);
    return cfg_err;
  }

  *out_repo = repo;
  return VXCORE_OK;
}

VxCoreError BootstrapFromEmptyRemote(const std::string &root_folder,
                                     const std::string &git_dir,
                                     const SyncConfig &config,
                                     const SyncCredentials &credentials,
                                     git_rebase **rebase_in_progress,
                                     git_repository **out_repo) {
  *out_repo = nullptr;

  git_repository *repo = nullptr;
  VxCoreError init_err = InitEmptyRepoWithRemote(
      root_folder, git_dir, config, credentials, rebase_in_progress,
      "Initialize(clone)", &repo);
  if (init_err != VXCORE_OK) {
    return init_err;
  }

  // Re-acquire the origin remote handle for the fetch — InitEmptyRepoWithRemote
  // freed its create-time handle to keep the helper symmetric.
  git_remote *remote = nullptr;
  int rc = git_remote_lookup(&remote, repo, "origin");
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_repository_free(repo);
    return err;
  }

  git_fetch_options fopts = GIT_FETCH_OPTIONS_INIT;
  GitCredentialPayload pl{credentials.personal_access_token};
  fopts.callbacks = MakeRemoteCallbacks(&pl);

  rc = git_remote_fetch(remote, /*refspecs=*/nullptr, &fopts,
                        "vnote initial fetch");
  git_remote_free(remote);
  remote = nullptr;
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_repository_free(repo);
    return err;
  }

  // Resolve the remote default branch. Prefer main, fall back to master.
  // If neither exists, the remote is truly empty (no commits yet) — leave
  // the local repo as-is with HEAD symbolically pointing at refs/heads/main
  // and let the future Sync() create the first commit.
  git_reference *origin_ref = nullptr;
  rc = git_reference_lookup(&origin_ref, repo, "refs/remotes/origin/main");
  if (rc != 0) {
    git_error_clear();
    rc = git_reference_lookup(&origin_ref, repo,
                              "refs/remotes/origin/master");
  }
  if (rc != 0) {
    git_error_clear();
    *out_repo = repo;
    return VXCORE_OK;
  }

  git_object *target = nullptr;
  rc = git_reference_peel(&target, origin_ref, GIT_OBJECT_COMMIT);
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_reference_free(origin_ref);
    git_repository_free(repo);
    return err;
  }

  git_checkout_options copts = GIT_CHECKOUT_OPTIONS_INIT;
  copts.checkout_strategy = GIT_CHECKOUT_SAFE;
  rc = git_checkout_tree(repo, target, &copts);
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_object_free(target);
    git_reference_free(origin_ref);
    git_repository_free(repo);
    return err;
  }

  // Snapshot the OID before freeing the peeled object, then create the
  // local refs/heads/main pointing at it. HEAD was set to a symbolic
  // refs/heads/main by init_ext; this binding makes it resolve.
  git_oid target_oid = *git_object_id(target);
  git_object_free(target);
  git_reference_free(origin_ref);

  git_reference *local_ref = nullptr;
  rc = git_reference_create(&local_ref, repo, "refs/heads/main",
                            &target_oid, /*force=*/0,
                            "vnote initial checkout");
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_repository_free(repo);
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
  ScrubGitlink(root_folder, "Initialize(clone)");

  *out_repo = repo;
  return VXCORE_OK;
}

VxCoreError BootstrapToEmptyRemote(const std::string &root_folder,
                                   const std::string &git_dir,
                                   const SyncConfig &config,
                                   const SyncCredentials &credentials,
                                   git_rebase **rebase_in_progress,
                                   git_repository **out_repo) {
  *out_repo = nullptr;

  git_repository *repo = nullptr;
  VxCoreError init_err = InitEmptyRepoWithRemote(
      root_folder, git_dir, config, credentials, rebase_in_progress,
      "Initialize(initpush)", &repo);
  if (init_err != VXCORE_OK) {
    return init_err;
  }

  GitSyncPipeline pipeline(repo, git_dir, root_folder, config, credentials,
                           rebase_in_progress);

  // Stage everything currently on disk (user files + just-written
  // .gitignore/.gitattributes), commit as the initial root commit, then
  // push refs/heads/main so the empty remote becomes the source of truth.
  // Helpers assume op_mutex_ already held — Initialize holds it for the
  // whole call so don't re-lock.
  VxCoreError err = pipeline.StageAll();
  if (err != VXCORE_OK) {
    git_repository_free(repo);
    return err;
  }
  err = pipeline.CommitIndex("VNote sync: initial commit");
  if (err != VXCORE_OK) {
    git_repository_free(repo);
    return err;
  }
  err = pipeline.PushOrigin();
  if (err != VXCORE_OK) {
    git_repository_free(repo);
    return err;
  }

  // Same gitlink scrub as Branch 2 (clone): libgit2 leaves a .git gitlink
  // file in the workdir pointing at our separate gitdir. VNote re-opens via
  // the gitdir directly, so we don't want notebooks to look like git working
  // trees to external tools. Only delete a regular file (the gitlink), never
  // a real user `.git` directory.
  ScrubGitlink(root_folder, "Initialize(initpush)");

  *out_repo = repo;
  return VXCORE_OK;
}

}  // namespace vxcore
