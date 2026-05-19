#include "sync/git/git_sync_pipeline.h"

#include <cstring>
#include <vector>

#include "sync/git/git_error_translator.h"
#include "utils/logger.h"

namespace vxcore {

GitSyncPipeline::GitSyncPipeline(git_repository *repo, const std::string &git_dir,
                                 const std::string &root_folder, const SyncConfig &config,
                                 const SyncCredentials &credentials,
                                 git_rebase **rebase_in_progress)
    : repo_(repo),
      git_dir_(git_dir),
      root_folder_(root_folder),
      config_(config),
      credentials_(credentials),
      rebase_in_progress_(rebase_in_progress) {}

// T18: open the repo-level config and force the four invariants. Caller holds
// op_mutex_ and has repo_ open. Returns the first failing translation, or
// VXCORE_OK on full success. Always frees the cfg handle.
VxCoreError GitSyncPipeline::ApplyDefaultGitConfig() {
  (void)git_dir_;
  (void)root_folder_;
  if (repo_ == nullptr) {
    return VXCORE_ERR_UNKNOWN;
  }
  git_config *cfg = nullptr;
  int rc = git_repository_config(&cfg, repo_);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  const char *user_name =
      credentials_.author_name.empty() ? "VNote Sync" : credentials_.author_name.c_str();
  const char *user_email =
      credentials_.author_email.empty() ? "sync@vnote.local" : credentials_.author_email.c_str();

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
bool GitSyncPipeline::RemoteHasRefs() {
  if (config_.remote_url.empty()) {
    return false;
  }

  git_remote *remote = nullptr;
  int rc = git_remote_create_detached(&remote, config_.remote_url.c_str());
  if (rc != 0) {
    const git_error *err = git_error_last();
    VXCORE_LOG_ERROR("RemoteHasRefs: git_remote_create_detached failed rc=%d: %s", rc,
                     (err && err->message) ? err->message : "(no message)");
    return false;
  }

  git_remote_callbacks cb = GIT_REMOTE_CALLBACKS_INIT;
  GitCredentialPayload pl{credentials_.personal_access_token};
  cb = MakeRemoteCallbacks(&pl);

  rc = git_remote_connect(remote, GIT_DIRECTION_FETCH, &cb,
                          /*proxy_opts=*/nullptr, /*custom_headers=*/nullptr);
  if (rc != 0) {
    const git_error *err = git_error_last();
    VXCORE_LOG_ERROR("RemoteHasRefs: git_remote_connect failed rc=%d: %s", rc,
                     (err && err->message) ? err->message : "(no message)");
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
    VXCORE_LOG_ERROR("RemoteHasRefs: git_remote_ls failed rc=%d: %s", rc,
                     (err && err->message) ? err->message : "(no message)");
  }

  git_remote_disconnect(remote);
  git_remote_free(remote);
  return has_refs;
}

// T21: stage every change under the working tree, then defensively scrub any
// path under vx_notebook/vx_sync/ from the index (the gitignore covers it,
// but we belt-and-brace because users may pass exclude_paths that override).
// Caller holds op_mutex_ and has repo_ open.
//
// libgit2 v1.7.x's git_index_add_all flat-out refuses any path under what it
// recognizes as a gitdir (returns "invalid path" with klass GIT_ERROR_INVALID
// the first time it walks vx_notebook/vx_sync/). To survive a missing or
// corrupted .gitignore we install a matched-path callback that skips those
// paths during the walk; the post-walk index scrub then mops up any stale
// entries (e.g. left over from a previous backend version that did stage
// them before this guard existed).
VxCoreError GitSyncPipeline::StageAll() {
  if (repo_ == nullptr) {
    return VXCORE_ERR_UNKNOWN;
  }

  git_index *idx = nullptr;
  int rc = git_repository_index(&idx, repo_);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  struct AddCbCtx {};
  auto add_cb = [](const char *path, const char *matched_pathspec, void *payload) -> int {
    (void)matched_pathspec;
    (void)payload;
    static const char kPrefix[] = "vx_notebook/vx_sync";
    if (path != nullptr && std::strncmp(path, kPrefix, sizeof(kPrefix) - 1) == 0) {
      return 1;  // skip
    }
    return 0;  // include
  };

  rc = git_index_add_all(idx, /*pathspec=*/nullptr, GIT_INDEX_ADD_DEFAULT, add_cb,
                         /*payload=*/nullptr);
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_index_free(idx);
    return err;
  }
  {
    size_t staged_count = git_index_entrycount(idx);
    VXCORE_LOG_DEBUG("GitSyncBackend::StageAll: index now has %zu entries", staged_count);
  }

  // Defensive removal: collect entry paths under vx_notebook/vx_sync first
  // (avoid mutating while iterating), then drop them by path/stage 0.
  static const std::string kExcludePrefix = "vx_notebook/vx_sync";
  std::vector<std::string> to_remove;
  const size_t n = git_index_entrycount(idx);
  for (size_t i = 0; i < n; ++i) {
    const git_index_entry *e = git_index_get_byindex(idx, i);
    if (e == nullptr || e->path == nullptr) {
      continue;
    }
    const std::string p(e->path);
    if (p.compare(0, kExcludePrefix.size(), kExcludePrefix) == 0) {
      to_remove.push_back(p);
    }
  }
  for (const auto &p : to_remove) {
    int rrc = git_index_remove(idx, p.c_str(), /*stage=*/0);
    if (rrc != 0) {
      VxCoreError err = TranslateGitError(rrc);
      git_index_free(idx);
      return err;
    }
  }

  rc = git_index_write(idx);
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_index_free(idx);
    return err;
  }

  git_index_free(idx);
  return VXCORE_OK;
}

// T22: if the index's tree differs from HEAD's tree, create a commit on HEAD
// using credentials_.author_name/_email (or the VNote defaults). If HEAD
// doesn't exist yet (initial commit), the new commit becomes the root.
// Returns VXCORE_OK both when a commit was made and when there was nothing
// to commit (empty-commit suppression).
VxCoreError GitSyncPipeline::CommitIndex(const std::string &message) {
  if (repo_ == nullptr) {
    return VXCORE_ERR_UNKNOWN;
  }

  git_index *idx = nullptr;
  int rc = git_repository_index(&idx, repo_);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  git_oid tree_oid;
  rc = git_index_write_tree(&tree_oid, idx);
  git_index_free(idx);
  idx = nullptr;
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  // Look up parent commit + its tree (if HEAD resolves) so we can suppress
  // empty commits and supply a parent to git_commit_create_v.
  git_object *parent_obj = nullptr;
  int head_rc = git_revparse_single(&parent_obj, repo_, "HEAD");
  git_commit *parent_commit = nullptr;
  bool have_parent = false;
  if (head_rc == 0) {
    rc = git_commit_lookup(&parent_commit, repo_, git_object_id(parent_obj));
    git_object_free(parent_obj);
    parent_obj = nullptr;
    if (rc != 0) {
      return TranslateGitError(rc);
    }
    have_parent = true;

    // Compare trees; suppress empty commit when identical.
    git_tree *parent_tree = nullptr;
    rc = git_commit_tree(&parent_tree, parent_commit);
    if (rc != 0) {
      VxCoreError err = TranslateGitError(rc);
      git_commit_free(parent_commit);
      return err;
    }
    const git_oid *parent_tree_oid = git_tree_id(parent_tree);
    if (parent_tree_oid != nullptr && git_oid_equal(parent_tree_oid, &tree_oid) != 0) {
      git_tree_free(parent_tree);
      git_commit_free(parent_commit);
      return VXCORE_OK;  // Nothing to commit.
    }
    git_tree_free(parent_tree);
  } else {
    // No HEAD yet — initial commit. Clear the "not found" libgit2 state so
    // a later TranslateGitError doesn't pick it up.
    git_error_clear();
  }

  const char *user_name =
      credentials_.author_name.empty() ? "VNote Sync" : credentials_.author_name.c_str();
  const char *user_email =
      credentials_.author_email.empty() ? "sync@vnote.local" : credentials_.author_email.c_str();

  git_signature *sig = nullptr;
  rc = git_signature_now(&sig, user_name, user_email);
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    if (parent_commit) git_commit_free(parent_commit);
    return err;
  }

  git_tree *tree = nullptr;
  rc = git_tree_lookup(&tree, repo_, &tree_oid);
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_signature_free(sig);
    if (parent_commit) git_commit_free(parent_commit);
    return err;
  }

  git_oid commit_oid;
  if (have_parent) {
    rc = git_commit_create_v(&commit_oid, repo_, "HEAD", sig, sig,
                             /*message_encoding=*/nullptr, message.c_str(), tree,
                             /*parent_count=*/1, parent_commit);
  } else {
    rc = git_commit_create_v(&commit_oid, repo_, "HEAD", sig, sig,
                             /*message_encoding=*/nullptr, message.c_str(), tree,
                             /*parent_count=*/0);
  }

  git_tree_free(tree);
  git_signature_free(sig);
  if (parent_commit) git_commit_free(parent_commit);
  if (rc != 0) {
    return TranslateGitError(rc);
  }
  return VXCORE_OK;
}

// T23: fetch refs from origin with the credential callback wired in so
// PAT-authenticated remotes work. Caller holds op_mutex_ and has repo_ open.
VxCoreError GitSyncPipeline::FetchOrigin() {
  if (repo_ == nullptr) {
    return VXCORE_ERR_UNKNOWN;
  }

  git_remote *remote = nullptr;
  int rc = git_remote_lookup(&remote, repo_, "origin");
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  git_fetch_options fopts = GIT_FETCH_OPTIONS_INIT;
  GitCredentialPayload pl{credentials_.personal_access_token};
  fopts.callbacks = MakeRemoteCallbacks(&pl);

  rc = git_remote_fetch(remote, /*refspecs=*/nullptr, &fopts, "vnote sync fetch");
  git_remote_free(remote);
  if (rc != 0) {
    return TranslateGitError(rc);
  }
  return VXCORE_OK;
}

// T24: rebase the current local branch onto origin/<branch>.
VxCoreError GitSyncPipeline::RebaseOntoOrigin() {
  if (repo_ == nullptr) {
    return VXCORE_ERR_UNKNOWN;
  }

  // Resolve current HEAD ref. UNBORNBRANCH means there are no local commits
  // yet (e.g. fresh init+empty remote) — nothing to rebase.
  git_reference *head_ref = nullptr;
  int rc = git_repository_head(&head_ref, repo_);
  if (rc == GIT_EUNBORNBRANCH) {
    git_error_clear();
    return VXCORE_OK;
  }
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  const char *branch_short = git_reference_shorthand(head_ref);
  if (branch_short == nullptr) {
    git_reference_free(head_ref);
    return VXCORE_ERR_UNKNOWN;
  }
  std::string remote_ref_name = std::string("refs/remotes/origin/") + branch_short;

  git_reference *remote_ref = nullptr;
  rc = git_reference_lookup(&remote_ref, repo_, remote_ref_name.c_str());
  if (rc == GIT_ENOTFOUND) {
    // No upstream tracking ref yet — push will publish the branch later.
    git_error_clear();
    git_reference_free(head_ref);
    return VXCORE_OK;
  }
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_reference_free(head_ref);
    return err;
  }

  // ===== Pre-check: skip rebase if not needed =====
  // libgit2's git_rebase_init unconditionally resets the working tree to the
  // onto commit BEFORE replaying — even when there's nothing to replay. When
  // it then fails mid-flight (e.g., GIT_ELOCKED on internal HEAD updates),
  // the working tree is left mid-reset with files deleted. Avoid the whole
  // class of failures by detecting trivial cases via merge-base.
  const git_oid *local_oid = git_reference_target(head_ref);
  const git_oid *remote_oid = git_reference_target(remote_ref);
  if (local_oid == nullptr || remote_oid == nullptr) {
    VXCORE_LOG_WARN("RebaseOntoOrigin: null OID from reference target, falling through");
  } else {
    git_oid merge_base_oid;
    int mb_rc = git_merge_base(&merge_base_oid, repo_, local_oid, remote_oid);
    if (mb_rc == 0) {
      const bool base_eq_local = (git_oid_cmp(&merge_base_oid, local_oid) == 0);
      const bool base_eq_remote = (git_oid_cmp(&merge_base_oid, remote_oid) == 0);

      if (base_eq_local && base_eq_remote) {
        VXCORE_LOG_DEBUG("RebaseOntoOrigin: in sync (local == remote), skipping rebase");
        git_reference_free(remote_ref);
        git_reference_free(head_ref);
        return VXCORE_OK;
      }
      if (base_eq_remote && !base_eq_local) {
        VXCORE_LOG_DEBUG(
            "RebaseOntoOrigin: local ahead of remote (no rebase needed, push will publish)");
        git_reference_free(remote_ref);
        git_reference_free(head_ref);
        return VXCORE_OK;
      }
      if (base_eq_local && !base_eq_remote) {
        VXCORE_LOG_DEBUG("RebaseOntoOrigin: remote ahead of local (fast-forward)");
        git_object *remote_obj = nullptr;
        int lookup_rc = git_object_lookup(&remote_obj, repo_, remote_oid, GIT_OBJECT_COMMIT);
        if (lookup_rc != 0) {
          VxCoreError err = TranslateGitError(lookup_rc);
          git_reference_free(remote_ref);
          git_reference_free(head_ref);
          return err;
        }
        git_checkout_options co_opts = GIT_CHECKOUT_OPTIONS_INIT;
        co_opts.checkout_strategy = GIT_CHECKOUT_FORCE;
        int co_rc = git_checkout_tree(repo_, remote_obj, &co_opts);
        git_object_free(remote_obj);
        if (co_rc != 0) {
          VxCoreError err = TranslateGitError(co_rc);
          git_reference_free(remote_ref);
          git_reference_free(head_ref);
          return err;
        }
        git_reference *new_local_ref = nullptr;
        int set_rc = git_reference_set_target(&new_local_ref, head_ref, remote_oid,
                                              "vnote: fast-forward to origin");
        git_reference_free(new_local_ref);
        git_reference_free(remote_ref);
        git_reference_free(head_ref);
        if (set_rc != 0) {
          return TranslateGitError(set_rc);
        }
        return VXCORE_OK;
      }
      VXCORE_LOG_DEBUG("RebaseOntoOrigin: true divergence detected, running rebase");
    } else if (mb_rc == GIT_ENOTFOUND) {
      git_error_clear();
      VXCORE_LOG_DEBUG("RebaseOntoOrigin: no merge-base (unrelated histories), running rebase");
    } else {
      VXCORE_LOG_WARN("RebaseOntoOrigin: git_merge_base failed rc=%d, falling through", mb_rc);
    }
  }
  // ===== End pre-check =====

  git_annotated_commit *local_anno = nullptr;
  rc = git_annotated_commit_from_ref(&local_anno, repo_, head_ref);
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_reference_free(remote_ref);
    git_reference_free(head_ref);
    return err;
  }

  git_annotated_commit *remote_anno = nullptr;
  rc = git_annotated_commit_from_ref(&remote_anno, repo_, remote_ref);
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_annotated_commit_free(local_anno);
    git_reference_free(remote_ref);
    git_reference_free(head_ref);
    return err;
  }

  rc = git_rebase_init(rebase_in_progress_, repo_, local_anno, remote_anno,
                       /*onto=*/nullptr, /*opts=*/nullptr);
  git_annotated_commit_free(local_anno);
  git_annotated_commit_free(remote_anno);
  git_reference_free(remote_ref);
  git_reference_free(head_ref);
  if (rc != 0) {
    *rebase_in_progress_ = nullptr;
    return TranslateGitError(rc);
  }

  // Default signature for new commits created during rebase replay.
  const char *user_name =
      credentials_.author_name.empty() ? "VNote Sync" : credentials_.author_name.c_str();
  const char *user_email =
      credentials_.author_email.empty() ? "sync@vnote.local" : credentials_.author_email.c_str();
  git_signature *sig = nullptr;
  rc = git_signature_now(&sig, user_name, user_email);
  if (rc != 0) {
    VxCoreError err = TranslateGitError(rc);
    git_rebase_free(*rebase_in_progress_);
    *rebase_in_progress_ = nullptr;
    return err;
  }

  // Replay each rebase operation. On conflict, leave rebase_in_progress_
  // set so ResolveConflict can resume (the on-disk rebase state under
  // .git/rebase-merge is also preserved).
  for (;;) {
    git_rebase_operation *op = nullptr;
    rc = git_rebase_next(&op, *rebase_in_progress_);
    if (rc == GIT_ITEROVER) {
      git_error_clear();
      rc = git_rebase_finish(*rebase_in_progress_, sig);
      git_rebase_free(*rebase_in_progress_);
      *rebase_in_progress_ = nullptr;
      git_signature_free(sig);
      if (rc != 0) {
        return TranslateGitError(rc);
      }
      return VXCORE_OK;
    }
    if (rc != 0) {
      VxCoreError err = TranslateGitError(rc);
      git_rebase_free(*rebase_in_progress_);
      *rebase_in_progress_ = nullptr;
      git_signature_free(sig);
      return err;
    }

    // Detect conflicts after each step.
    git_index *idx = nullptr;
    int irc = git_repository_index(&idx, repo_);
    if (irc != 0) {
      VxCoreError err = TranslateGitError(irc);
      git_rebase_free(*rebase_in_progress_);
      *rebase_in_progress_ = nullptr;
      git_signature_free(sig);
      return err;
    }
    bool has_conflicts = (git_index_has_conflicts(idx) != 0);
    git_index_free(idx);

    if (has_conflicts) {
      // LEAVE rebase_in_progress_ set; T29-T32 ResolveConflict will resume.
      git_signature_free(sig);
      return VXCORE_ERR_SYNC_CONFLICT;
    }

    git_oid commit_oid;
    rc = git_rebase_commit(&commit_oid, *rebase_in_progress_, /*author=*/sig,
                           /*committer=*/sig, /*message_encoding=*/nullptr,
                           /*message=*/nullptr);
    if (rc != 0) {
      VxCoreError err = TranslateGitError(rc);
      git_rebase_free(*rebase_in_progress_);
      *rebase_in_progress_ = nullptr;
      git_signature_free(sig);
      return err;
    }
  }
}

// T25: push the current branch back to origin. Refspec is built explicitly
// from git_repository_head's shorthand to avoid relying on configured
// upstream tracking. Caller must hold op_mutex_ and have repo_ open.
VxCoreError GitSyncPipeline::PushOrigin() {
  if (repo_ == nullptr) {
    return VXCORE_ERR_UNKNOWN;
  }

  git_remote *remote = nullptr;
  int rc = git_remote_lookup(&remote, repo_, "origin");
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  git_push_options popts = GIT_PUSH_OPTIONS_INIT;
  GitCredentialPayload pl{credentials_.personal_access_token};
  popts.callbacks = MakeRemoteCallbacks(&pl);

  git_reference *head_ref = nullptr;
  rc = git_repository_head(&head_ref, repo_);
  if (rc != 0) {
    git_remote_free(remote);
    return TranslateGitError(rc);
  }
  const char *branch_short = git_reference_shorthand(head_ref);
  if (branch_short == nullptr) {
    git_reference_free(head_ref);
    git_remote_free(remote);
    return VXCORE_ERR_UNKNOWN;
  }
  std::string refspec = std::string("refs/heads/") + branch_short + ":refs/heads/" + branch_short;
  git_reference_free(head_ref);

  // git_strarray takes a non-const char**; refspec.data() is valid since C++17.
  char *arr[1] = {refspec.data()};
  git_strarray refspecs = {arr, 1};

  rc = git_remote_push(remote, &refspecs, &popts);
  git_remote_free(remote);
  if (rc != 0) {
    return TranslateGitError(rc);
  }
  return VXCORE_OK;
}

// T29-T32: continue an in-progress rebase after the caller has staged the
// resolved file and removed the conflict from the index. Returns OK if no
// rebase is in progress (idempotent). On a fresh conflict during replay,
// leaves rebase_in_progress_ set and returns OK so the caller can resolve
// the next conflict (matches RebaseOntoOrigin's "leave it for ResolveConflict
// to resume" contract — but ResolveConflict signals OK because the user's
// requested resolution did succeed; the next GetConflicts call will surface
// the remaining unresolved files).
VxCoreError GitSyncPipeline::ContinueRebaseAfterResolution() {
  if (repo_ == nullptr) {
    return VXCORE_ERR_UNKNOWN;
  }
  if (*rebase_in_progress_ == nullptr) {
    return VXCORE_OK;
  }
  if (git_repository_state(repo_) != GIT_REPOSITORY_STATE_REBASE_MERGE) {
    return VXCORE_OK;
  }

  const char *user_name =
      credentials_.author_name.empty() ? "VNote Sync" : credentials_.author_name.c_str();
  const char *user_email =
      credentials_.author_email.empty() ? "sync@vnote.local" : credentials_.author_email.c_str();
  git_signature *sig = nullptr;
  int rc = git_signature_now(&sig, user_name, user_email);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  // Commit the just-resolved operation. GIT_EAPPLIED means the resolution
  // produced an empty commit (e.g. KeepRemote on an exact match) — treat as
  // success and continue.
  git_oid commit_oid;
  rc = git_rebase_commit(&commit_oid, *rebase_in_progress_, /*author=*/sig,
                         /*committer=*/sig, /*message_encoding=*/nullptr,
                         /*message=*/nullptr);
  if (rc != 0 && rc != GIT_EAPPLIED) {
    VxCoreError err = TranslateGitError(rc);
    git_signature_free(sig);
    return err;
  }
  git_error_clear();

  for (;;) {
    git_rebase_operation *op = nullptr;
    rc = git_rebase_next(&op, *rebase_in_progress_);
    if (rc == GIT_ITEROVER) {
      git_error_clear();
      rc = git_rebase_finish(*rebase_in_progress_, sig);
      git_rebase_free(*rebase_in_progress_);
      *rebase_in_progress_ = nullptr;
      git_signature_free(sig);
      if (rc != 0) {
        return TranslateGitError(rc);
      }
      return VXCORE_OK;
    }
    if (rc != 0) {
      VxCoreError err = TranslateGitError(rc);
      git_rebase_free(*rebase_in_progress_);
      *rebase_in_progress_ = nullptr;
      git_signature_free(sig);
      return err;
    }

    git_index *idx = nullptr;
    int irc = git_repository_index(&idx, repo_);
    if (irc != 0) {
      VxCoreError err = TranslateGitError(irc);
      git_rebase_free(*rebase_in_progress_);
      *rebase_in_progress_ = nullptr;
      git_signature_free(sig);
      return err;
    }
    bool has_conflicts = (git_index_has_conflicts(idx) != 0);
    git_index_free(idx);

    if (has_conflicts) {
      // Leave rebase_in_progress_ set; caller resolves next conflict.
      git_signature_free(sig);
      return VXCORE_OK;
    }

    rc = git_rebase_commit(&commit_oid, *rebase_in_progress_, /*author=*/sig,
                           /*committer=*/sig, /*message_encoding=*/nullptr,
                           /*message=*/nullptr);
    if (rc != 0 && rc != GIT_EAPPLIED) {
      VxCoreError err = TranslateGitError(rc);
      git_rebase_free(*rebase_in_progress_);
      *rebase_in_progress_ = nullptr;
      git_signature_free(sig);
      return err;
    }
    git_error_clear();
  }
}

}  // namespace vxcore
