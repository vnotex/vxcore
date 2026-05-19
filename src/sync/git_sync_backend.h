#ifndef VXCORE_GIT_SYNC_BACKEND_H
#define VXCORE_GIT_SYNC_BACKEND_H

#include "sync/sync_backend.h"
#include "sync/git/libgit2_init.h"
#include "vxcore/vxcore_types.h"

#include <mutex>
#include <string>

// Forward declarations to keep libgit2 types out of the public header.
typedef struct git_repository git_repository;
struct git_rebase;

namespace vxcore {

// Concrete ISyncBackend implementation backed by libgit2.
//
// Repository layout: <root_folder>/vx_notebook/vx_sync/ is the .git directory
// (separate gitdir), the notebook root is the working tree.
// All libgit2 operations are serialized by op_mutex_; concurrent Sync() returns
// VXCORE_ERR_SYNC_IN_PROGRESS via try_lock.
class GitSyncBackend : public ISyncBackend {
 public:
  VXCORE_API GitSyncBackend();
  VXCORE_API ~GitSyncBackend() override;

  GitSyncBackend(const GitSyncBackend&) = delete;
  GitSyncBackend& operator=(const GitSyncBackend&) = delete;
  GitSyncBackend(GitSyncBackend&&) = delete;
  GitSyncBackend& operator=(GitSyncBackend&&) = delete;

  // ISyncBackend
  VxCoreError SetCredentials(const SyncCredentials &creds) override;
  VxCoreError Initialize(const std::string &root_folder,
                         const SyncConfig &config) override;
  VxCoreError Shutdown() override;
  VxCoreError Sync(SyncProgressCallback callback, void *userdata) override;
  VxCoreError Push(SyncProgressCallback callback, void *userdata) override;
  VxCoreError Pull(SyncProgressCallback callback, void *userdata) override;
  VxCoreError GetStatus(std::vector<SyncFileInfo> &out_files) override;
  VxCoreError GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) override;
  VxCoreError ResolveConflict(const std::string &path,
                              SyncConflictResolution resolution) override;

  // Test-only helper: writes default .gitignore + .gitattributes (using
  // config.exclude_paths) into <dir>, preserving any pre-existing files.
  // Returns count of files actually written.
  // Internal callers (T15/T18) use the same helpers via Initialize.
  static VXCORE_API int WriteDefaultIgnoreAndAttributesForTesting(const std::string &dir,
                                                                  const SyncConfig &config);

  // T28 test hook: exposes the file-scope TranslateGitError translator so
  // unit tests can verify libgit2 error class -> VxCoreError mapping without
  // reaching into anonymous-namespace internals.
  static VXCORE_API VxCoreError TranslateGitErrorForTesting(int git_rc);

  // T21/T22/T23 test hooks: each acquires op_mutex_ and delegates to the
  // private helper. They allow unit tests to drive the Sync sub-steps in
  // isolation without exercising the full Sync() orchestration (which lands
  // in T24+T25+T26).
  VXCORE_API VxCoreError StageAllForTesting();
  VXCORE_API VxCoreError CommitIndexForTesting(const std::string &message);
  VXCORE_API VxCoreError FetchOriginForTesting();

  // T24/T25 test hooks: drive the rebase / push helpers in isolation. Each
  // acquires op_mutex_, checks initialized_, then delegates to the private
  // helper.
  VXCORE_API VxCoreError RebaseOntoOriginForTesting();
  VXCORE_API VxCoreError PushOriginForTesting();

 private:
  // T21: stage everything tracked + untracked under repo workdir, then
  // defensively remove any index entry under vx_notebook/vx_sync/ (our
  // gitdir lives there; .gitignore should already exclude it but we belt-
  // and-brace it). Caller must hold op_mutex_ and have repo_ open.
  VxCoreError StageAll();

  // T22: write the current index out as a tree, compare to HEAD's tree, and
  // create a commit on HEAD if the trees differ. Empty (no-op) commits are
  // suppressed and return VXCORE_OK. Author/committer come from
  // credentials_.author_name/_email with sensible defaults. Caller must
  // hold op_mutex_ and have repo_ open.
  VxCoreError CommitIndex(const std::string &message);

  // T23: git_remote_lookup("origin") + git_remote_fetch with our credential
  // callback wired in. Caller must hold op_mutex_ and have repo_ open.
  VxCoreError FetchOrigin();

  // T24: rebase the current branch onto refs/remotes/origin/<branch>. If HEAD
  // is unborn or the remote-tracking ref does not exist, returns OK (nothing
  // to rebase). On a conflict, leaves rebase_in_progress_ set so a later
  // ResolveConflict (T29-T32) can complete the rebase, and returns
  // VXCORE_ERR_SYNC_CONFLICT. Caller must hold op_mutex_ and have repo_ open.
  VxCoreError RebaseOntoOrigin();

  // T25: push the current branch to origin/<branch>. Wires the credential
  // callback so PAT auth works. Caller must hold op_mutex_ and have repo_
  // open.
  VxCoreError PushOrigin();

  // T29-T32: After a single conflict has been resolved by the caller (index
  // staged + conflict cleared), continue the in-progress rebase. Commits the
  // current operation via git_rebase_commit, then loops git_rebase_next:
  // on a fresh conflict leaves rebase_in_progress_ set and returns OK; on
  // GIT_ITEROVER calls git_rebase_finish and clears rebase_in_progress_.
  // No-op (returns OK) if no rebase is in progress. Caller must hold
  // op_mutex_ and have repo_ open.
  VxCoreError ContinueRebaseAfterResolution();

  // T18: apply baseline repo-level config (filemode, autocrlf, gpgsign,
  // user.name, user.email). Caller must hold op_mutex_ and have repo_ open.
  VxCoreError ApplyDefaultGitConfig();

  // T17 helper: returns true if the remote at config_.remote_url advertises
  // any refs. Uses an in-memory repo + anonymous remote with the credential
  // callback so PAT auth (if set) is honored. Logs and returns false on
  // network/auth failure (caller treats inability to reach as "no refs").
  bool RemoteHasRefs();

  LibGit2Init libgit2_;     // RAII; FIRST member so it's last destroyed
  std::mutex op_mutex_;     // serializes per-backend libgit2 calls
  std::string root_folder_; // notebook root (= libgit2 workdir)
  std::string git_dir_;     // root_folder_ + "/vx_notebook/vx_sync/"
  SyncConfig config_;
  SyncCredentials credentials_;
  git_repository *repo_ = nullptr;
  git_rebase *rebase_in_progress_ = nullptr; // T24 sets when rebase pauses on conflict
  bool initialized_ = false;
};

}  // namespace vxcore

#endif  // VXCORE_GIT_SYNC_BACKEND_H
