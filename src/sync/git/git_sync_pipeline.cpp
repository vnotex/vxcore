#include "sync/git/git_sync_pipeline.h"

#include <cstring>
#include <string>
#include <vector>

#include "sync/credential_provider.h"
#include "sync/git/git_defaults.h"
#include "sync/git/git_error_translator.h"
#include "sync/git/git_handles.h"
#include "utils/logger.h"

// ============================================================================
// GitSyncPipeline: Single-shot phase methods for git sync operations.
//
// AUTO-COMMIT BEHAVIOR (B5):
// The Pull() path auto-commits staged changes before fetch+rebase via
// CommitIndex("VNote sync auto-commit before pull"). This ensures uncommitted
// local edits survive the rebase. The auto-commit is GATED by
// SyncConfig::auto_commit_merges (default true to preserve current behavior).
//
// When auto_commit_merges is false:
//   - Pull() stages changes but skips the commit
//   - Staged changes remain in the index for the caller to handle
//   - Rebase proceeds with a clean working tree (staged changes are committed
//     by the rebase itself as part of the replay)
//   - Caller is responsible for managing the staged state
//
// When auto_commit_merges is true (default):
//   - Pull() stages and auto-commits before rebase (current behavior)
//   - Rebase replays on top of the auto-commit
//   - Caller sees a clean working tree after Pull()
//
// CREDENTIAL HANDLING (Wave 6.3 F4.4 of sync-backend-phase4):
// The pipeline holds a non-owning ICredentialProvider* (its lifetime is
// the caller's responsibility; backend keeps a shared_ptr alive on the
// stack across the pipeline's scope). Each phase that touches the network
// invokes MakeRemoteCallbacks(provider, url) which takes a FRESH snapshot
// at callback-build time. The commit-author identity (user.name / user.email)
// is snapshotted ONCE at ctor and reused for ApplyDefaultGitConfig +
// CommitIndex + RebaseOntoOrigin + ContinueRebaseAfterResolution so all
// commits made through one pipeline use a stable signature even if the
// provider is rotated mid-sync.
// ============================================================================

namespace vxcore {

namespace {

// Workaround for libgit2 WinHTTP credential-callback failures against real
// GitHub HTTPS. Embeds the PAT directly into the URL so WinHTTP picks up
// Basic auth via URL userinfo parsing instead of relying on credential
// callback negotiation. NEVER logs the result.
//
// Returns the input URL unchanged when:
//   - PAT is empty
//   - URL is not https://
//   - URL already contains userinfo (won't double-embed)
//
// NOTE: assumes the GitHub PAT character set ([A-Za-z0-9_]); URL-encode
// the PAT if generalizing this to providers whose secrets may contain
// '@', ':', '/', '?', '#', or '%'.
//
// The returned URL stays in-process only — it is only used as the in-memory
// remote instance URL for one libgit2 operation; never persisted to disk.
static std::string MaybeEmbedPatInUrl(const std::string &url, const std::string &pat) {
  if (pat.empty()) return url;
  if (url.compare(0, 8, "https://") != 0) return url;
  const size_t scheme_end = 8;  // length of "https://"
  const size_t first_slash = url.find('/', scheme_end);
  const size_t at_pos = url.find('@', scheme_end);
  if (at_pos != std::string::npos &&
      (first_slash == std::string::npos || at_pos < first_slash)) {
    return url;  // already has userinfo
  }
  return std::string("https://x-access-token:") + pat + "@" + url.substr(scheme_end);
}

// Drive a libgit2 rebase replay loop that has already been initialised
// (`git_rebase_init` performed, `rebase` non-null) and whose signature is
// owned by the caller. Steps repeatedly until either (a) `git_rebase_next`
// returns `GIT_ITEROVER` — at which point the rebase is finished, freed,
// and `*rebase_out` is reset to nullptr, or (b) the index ends up with
// conflicts after a step — at which point the rebase is LEFT in progress
// (caller's contract with ResolveConflict) and the supplied
// `on_conflict_result` value is returned verbatim.
//
// `allow_eapplied` toggles whether `git_rebase_commit` returning
// `GIT_EAPPLIED` (the resolution produced an empty commit, e.g. KeepRemote
// on an exact match) is treated as success. Fresh `RebaseOntoOrigin`
// replays never observe GIT_EAPPLIED so they pass `false`;
// `ContinueRebaseAfterResolution` passes `true`.
//
// NOTE: `rebase_out` is a BORROWED pointer managed by the caller
// (GitSyncBackend's rebase_in_progress_ member). The git_rebase_free()
// calls in this function are part of the borrowed-pointer contract and
// must NOT be replaced with RAII.
//
// Behaviour quirks preserved verbatim from the two pre-refactor call sites:
//   - On any non-iterover/non-conflict error, the rebase IS freed and
//     `*rebase_out` set to nullptr before returning. Caller does NOT need
//     to clean up.
//   - On conflict-hit, the rebase is INTENTIONALLY left in place — both
//     the in-memory handle and the on-disk `.git/rebase-merge/` state —
//     so the conflict resolver can `git_rebase_commit` after resolution.
//   - `git_error_clear()` is called on `GIT_ITEROVER` to match the
//     pre-refactor blocks (libgit2 leaves a non-fatal error string set).
VxCoreError DriveRebaseLoop(git_repository *repo, git_rebase **rebase_out,
                            git_signature *sig, bool allow_eapplied,
                            VxCoreError on_conflict_result) {
  for (;;) {
    git_rebase_operation *op = nullptr;
    int rc = git_rebase_next(&op, *rebase_out);
    if (rc == GIT_ITEROVER) {
      git_error_clear();
      rc = git_rebase_finish(*rebase_out, sig);
      git_rebase_free(*rebase_out);  // BORROWED: keep as-is
      *rebase_out = nullptr;
      if (rc != 0) {
        return TranslateGitError(rc);
      }
      return VXCORE_OK;
    }
    if (rc != 0) {
      VxCoreError err = TranslateGitError(rc);
      git_rebase_free(*rebase_out);  // BORROWED: keep as-is
      *rebase_out = nullptr;
      return err;
    }

    git_index *raw_idx = nullptr;
    int irc = git_repository_index(&raw_idx, repo);
    git_indexPtr idx(raw_idx);
    if (irc != 0) {
      VxCoreError err = TranslateGitError(irc);
      git_rebase_free(*rebase_out);  // BORROWED: keep as-is
      *rebase_out = nullptr;
      return err;
    }
    bool has_conflicts = (git_index_has_conflicts(idx.get()) != 0);
    idx.reset();

    if (has_conflicts) {
      // LEAVE *rebase_out set; caller's ResolveConflict will resume.
      return on_conflict_result;
    }

    git_oid commit_oid;
    rc = git_rebase_commit(&commit_oid, *rebase_out, /*author=*/sig,
                           /*committer=*/sig, /*message_encoding=*/nullptr,
                           /*message=*/nullptr);
    if (rc != 0 && !(allow_eapplied && rc == GIT_EAPPLIED)) {
      VxCoreError err = TranslateGitError(rc);
      git_rebase_free(*rebase_out);  // BORROWED: keep as-is
      *rebase_out = nullptr;
      return err;
    }
    if (allow_eapplied) {
      git_error_clear();
    }
  }
}

// Snapshot author identity from a provider at pipeline-construction time.
// If the provider is null or declines, the snapshot fields stay empty
// (the consuming methods then fall back to kDefaultAuthorName/Email).
void SnapshotAuthor(ICredentialProvider *provider, const std::string &remote_url,
                    std::string *out_name, std::string *out_email) {
  if (provider == nullptr) {
    return;
  }
  SyncCredentials snapshot;
  if (provider->GetCredentials(remote_url, /*username_from_url=*/"x-access-token",
                               &snapshot)) {
    *out_name = snapshot.author_name;
    *out_email = snapshot.author_email;
  }
}

}  // namespace

GitSyncPipeline::GitSyncPipeline(git_repository *repo, const std::string &git_dir,
                                 const std::string &root_folder, const SyncConfig &config,
                                 std::shared_ptr<ICredentialProvider> creds_provider,
                                 git_rebase **rebase_in_progress)
    : repo_(repo),
      git_dir_(git_dir),
      root_folder_(root_folder),
      config_(config),
      creds_provider_(std::move(creds_provider)),
      rebase_in_progress_(rebase_in_progress) {
  // Wave 6.3 F4.4: stable per-pipeline author identity snapshot. Defaults
  // to empty (-> built-in VNote defaults at use site) when no provider is
  // supplied or when the provider declines.
  SnapshotAuthor(creds_provider_.get(), config_.remote_url, &author_name_, &author_email_);
}

void GitSyncPipeline::SetCancellation(SyncCancellationPtr token) {
  cancellation_ = std::move(token);
}

// T18: open the repo-level config and force the four invariants. Caller holds
// op_mutex_ and has repo_ open. Returns the first failing translation, or
// VXCORE_OK on full success. RAII frees the cfg handle.
VxCoreError GitSyncPipeline::ApplyDefaultGitConfig() {
  (void)git_dir_;
  (void)root_folder_;
  if (repo_ == nullptr) {
    return VXCORE_ERR_UNKNOWN;
  }
  git_config *raw_cfg = nullptr;
  int rc = git_repository_config(&raw_cfg, repo_);
  git_configPtr cfg(raw_cfg);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  const char *user_name =
      author_name_.empty() ? kDefaultAuthorName : author_name_.c_str();
  const char *user_email =
      author_email_.empty() ? kDefaultAuthorEmail : author_email_.c_str();

  // core.autocrlf is set as a string ("false") to match git's textual config
  // representation; the others are booleans.
  rc = git_config_set_bool(cfg.get(), "core.filemode", 0);
  if (rc == 0) rc = git_config_set_string(cfg.get(), "core.autocrlf", "false");
  if (rc == 0) rc = git_config_set_bool(cfg.get(), "commit.gpgsign", 0);
  if (rc == 0) rc = git_config_set_string(cfg.get(), "user.name", user_name);
  if (rc == 0) rc = git_config_set_string(cfg.get(), "user.email", user_email);

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

  std::string remote_url_for_call = config_.remote_url;
  {
    SyncCredentials cred_snapshot;
    if (creds_provider_ &&
        creds_provider_->GetCredentials(config_.remote_url, "x-access-token",
                                        &cred_snapshot) &&
        !cred_snapshot.personal_access_token.empty()) {
      remote_url_for_call =
          MaybeEmbedPatInUrl(config_.remote_url, cred_snapshot.personal_access_token);
    }
  }
  git_remote *raw_remote = nullptr;
  int rc = git_remote_create_detached(&raw_remote, remote_url_for_call.c_str());
  git_remotePtr remote(raw_remote);
  if (rc != 0) {
    const git_error *err = git_error_last();
    VXCORE_LOG_ERROR("RemoteHasRefs: git_remote_create_detached failed rc=%d: %s", rc,
                     (err && err->message) ? err->message : "(no message)");
    return false;
  }

  auto bundle = MakeRemoteCallbacks(creds_provider_.get(), config_.remote_url, cancellation_.get());
  git_remote_callbacks cb = bundle.callbacks;

  rc = git_remote_connect(remote.get(), GIT_DIRECTION_FETCH, &cb,
                          /*proxy_opts=*/nullptr, /*custom_headers=*/nullptr);
  if (rc != 0) {
    const git_error *err = git_error_last();
    VXCORE_LOG_ERROR("RemoteHasRefs: git_remote_connect failed rc=%d: %s", rc,
                     (err && err->message) ? err->message : "(no message)");
    return false;
  }

  const git_remote_head **heads = nullptr;
  size_t n_heads = 0;
  rc = git_remote_ls(&heads, &n_heads, remote.get());
  bool has_refs = false;
  if (rc == 0) {
    has_refs = (n_heads > 0);
  } else {
    const git_error *err = git_error_last();
    VXCORE_LOG_ERROR("RemoteHasRefs: git_remote_ls failed rc=%d: %s", rc,
                     (err && err->message) ? err->message : "(no message)");
  }

  git_remote_disconnect(remote.get());
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

  git_index *raw_idx = nullptr;
  int rc = git_repository_index(&raw_idx, repo_);
  git_indexPtr idx(raw_idx);
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

  rc = git_index_add_all(idx.get(), /*pathspec=*/nullptr, GIT_INDEX_ADD_DEFAULT, add_cb,
                         /*payload=*/nullptr);
  if (rc != 0) {
    return TranslateGitError(rc);
  }
  {
    size_t staged_count = git_index_entrycount(idx.get());
    VXCORE_LOG_DEBUG("GitSyncBackend::StageAll: index now has %zu entries", staged_count);
  }

  // Defensive removal: collect entry paths under vx_notebook/vx_sync first
  // (avoid mutating while iterating), then drop them by path/stage 0.
  static const std::string kExcludePrefix = "vx_notebook/vx_sync";
  std::vector<std::string> to_remove;
  const size_t n = git_index_entrycount(idx.get());
  for (size_t i = 0; i < n; ++i) {
    const git_index_entry *e = git_index_get_byindex(idx.get(), i);
    if (e == nullptr || e->path == nullptr) {
      continue;
    }
    const std::string p(e->path);
    if (p.compare(0, kExcludePrefix.size(), kExcludePrefix) == 0) {
      to_remove.push_back(p);
    }
  }
  for (const auto &p : to_remove) {
    int rrc = git_index_remove(idx.get(), p.c_str(), /*stage=*/0);
    if (rrc != 0) {
      return TranslateGitError(rrc);
    }
  }

  rc = git_index_write(idx.get());
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  return VXCORE_OK;
}

// T22: if the index's tree differs from HEAD's tree, create a commit on HEAD
// using author_name_/author_email_ (or the VNote defaults). If HEAD doesn't
// exist yet (initial commit), the new commit becomes the root.
// Returns VXCORE_OK both when a commit was made and when there was nothing
// to commit (empty-commit suppression).
VxCoreError GitSyncPipeline::CommitIndex(const std::string &message) {
  if (repo_ == nullptr) {
    return VXCORE_ERR_UNKNOWN;
  }

  git_oid tree_oid;
  {
    git_index *raw_idx = nullptr;
    int rc = git_repository_index(&raw_idx, repo_);
    git_indexPtr idx(raw_idx);
    if (rc != 0) {
      return TranslateGitError(rc);
    }

    rc = git_index_write_tree(&tree_oid, idx.get());
    if (rc != 0) {
      return TranslateGitError(rc);
    }
  }

  // Look up parent commit + its tree (if HEAD resolves) so we can suppress
  // empty commits and supply a parent to git_commit_create_v.
  git_commitPtr parent_commit;
  bool have_parent = false;
  {
    git_object *raw_parent_obj = nullptr;
    int head_rc = git_revparse_single(&raw_parent_obj, repo_, "HEAD");
    git_objectPtr parent_obj(raw_parent_obj);
    if (head_rc == 0) {
      git_commit *raw_parent_commit = nullptr;
      int rc = git_commit_lookup(&raw_parent_commit, repo_, git_object_id(parent_obj.get()));
      parent_commit.reset(raw_parent_commit);
      if (rc != 0) {
        return TranslateGitError(rc);
      }
      have_parent = true;

      // Compare trees; suppress empty commit when identical.
      git_tree *raw_parent_tree = nullptr;
      rc = git_commit_tree(&raw_parent_tree, parent_commit.get());
      git_treePtr parent_tree(raw_parent_tree);
      if (rc != 0) {
        return TranslateGitError(rc);
      }
      const git_oid *parent_tree_oid = git_tree_id(parent_tree.get());
      if (parent_tree_oid != nullptr && git_oid_equal(parent_tree_oid, &tree_oid) != 0) {
        return VXCORE_OK;  // Nothing to commit.
      }
    } else {
      // No HEAD yet — initial commit. Clear the "not found" libgit2 state so
      // a later TranslateGitError doesn't pick it up.
      git_error_clear();
    }
  }

  const char *user_name =
      author_name_.empty() ? kDefaultAuthorName : author_name_.c_str();
  const char *user_email =
      author_email_.empty() ? kDefaultAuthorEmail : author_email_.c_str();

  git_signature *raw_sig = nullptr;
  int rc = git_signature_now(&raw_sig, user_name, user_email);
  git_signaturePtr sig(raw_sig);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  git_tree *raw_tree = nullptr;
  rc = git_tree_lookup(&raw_tree, repo_, &tree_oid);
  git_treePtr tree(raw_tree);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  git_oid commit_oid;
  if (have_parent) {
    rc = git_commit_create_v(&commit_oid, repo_, "HEAD", sig.get(), sig.get(),
                             /*message_encoding=*/nullptr, message.c_str(), tree.get(),
                             /*parent_count=*/1, parent_commit.get());
  } else {
    rc = git_commit_create_v(&commit_oid, repo_, "HEAD", sig.get(), sig.get(),
                             /*message_encoding=*/nullptr, message.c_str(), tree.get(),
                             /*parent_count=*/0);
  }

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

  git_remote *raw_remote = nullptr;
  int rc = git_remote_lookup(&raw_remote, repo_, "origin");
  git_remotePtr remote(raw_remote);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  git_fetch_options fopts = GIT_FETCH_OPTIONS_INIT;
  // OPTION C WORKAROUND: override remote URL with PAT-embedded version for
  // this libgit2 operation only. Bypasses credential-callback issues on
  // Windows WinHTTP transport against real GitHub HTTPS. The on-disk
  // remote.origin.url config is UNCHANGED.
  {
    SyncCredentials cred_snapshot;
    if (creds_provider_ &&
        creds_provider_->GetCredentials(config_.remote_url, "x-access-token",
                                        &cred_snapshot) &&
        !cred_snapshot.personal_access_token.empty()) {
      const std::string embedded_url =
          MaybeEmbedPatInUrl(config_.remote_url, cred_snapshot.personal_access_token);
      if (embedded_url != config_.remote_url) {
        const int set_rc = git_remote_set_instance_url(remote.get(), embedded_url.c_str());
        if (set_rc != 0) {
          VXCORE_LOG_WARN("FetchOrigin: git_remote_set_instance_url failed rc=%d, falling back to callback path", set_rc);
        }
      }
    }
  }
  auto bundle = MakeRemoteCallbacks(creds_provider_.get(), config_.remote_url, cancellation_.get());
  fopts.callbacks = bundle.callbacks;

  rc = git_remote_fetch(remote.get(), /*refspecs=*/nullptr, &fopts, "vnote sync fetch");
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
  git_reference *raw_head_ref = nullptr;
  int rc = git_repository_head(&raw_head_ref, repo_);
  if (rc == GIT_EUNBORNBRANCH) {
    git_error_clear();
    return VXCORE_OK;
  }
  if (rc != 0) {
    return TranslateGitError(rc);
  }
  git_referencePtr head_ref(raw_head_ref);

  const char *branch_short = git_reference_shorthand(head_ref.get());
  if (branch_short == nullptr) {
    return VXCORE_ERR_UNKNOWN;
  }
  std::string remote_ref_name = std::string("refs/remotes/origin/") + branch_short;

  git_reference *raw_remote_ref = nullptr;
  rc = git_reference_lookup(&raw_remote_ref, repo_, remote_ref_name.c_str());
  if (rc == GIT_ENOTFOUND) {
    // No upstream tracking ref yet — push will publish the branch later.
    git_error_clear();
    return VXCORE_OK;
  }
  if (rc != 0) {
    return TranslateGitError(rc);
  }
  git_referencePtr remote_ref(raw_remote_ref);

  // ===== Pre-check: skip rebase if not needed =====
  // libgit2's git_rebase_init unconditionally resets the working tree to the
  // onto commit BEFORE replaying — even when there's nothing to replay. When
  // it then fails mid-flight (e.g., GIT_ELOCKED on internal HEAD updates),
  // the working tree is left mid-reset with files deleted. Avoid the whole
  // class of failures by detecting trivial cases via merge-base.
  const git_oid *local_oid = git_reference_target(head_ref.get());
  const git_oid *remote_oid = git_reference_target(remote_ref.get());
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
        return VXCORE_OK;
      }
      if (base_eq_remote && !base_eq_local) {
        VXCORE_LOG_DEBUG(
            "RebaseOntoOrigin: local ahead of remote (no rebase needed, push will publish)");
        return VXCORE_OK;
      }
      if (base_eq_local && !base_eq_remote) {
        VXCORE_LOG_DEBUG("RebaseOntoOrigin: remote ahead of local (fast-forward)");
        git_object *raw_remote_obj = nullptr;
        int lookup_rc = git_object_lookup(&raw_remote_obj, repo_, remote_oid, GIT_OBJECT_COMMIT);
        git_objectPtr remote_obj(raw_remote_obj);
        if (lookup_rc != 0) {
          return TranslateGitError(lookup_rc);
        }
        git_checkout_options co_opts = GIT_CHECKOUT_OPTIONS_INIT;
        co_opts.checkout_strategy = GIT_CHECKOUT_FORCE;
        int co_rc = git_checkout_tree(repo_, remote_obj.get(), &co_opts);
        if (co_rc != 0) {
          return TranslateGitError(co_rc);
        }
        git_reference *raw_new_local_ref = nullptr;
        int set_rc = git_reference_set_target(&raw_new_local_ref, head_ref.get(), remote_oid,
                                              "vnote: fast-forward to origin");
        git_referencePtr new_local_ref(raw_new_local_ref);
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

  git_annotated_commit *raw_local_anno = nullptr;
  rc = git_annotated_commit_from_ref(&raw_local_anno, repo_, head_ref.get());
  git_annotated_commitPtr local_anno(raw_local_anno);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  git_annotated_commit *raw_remote_anno = nullptr;
  rc = git_annotated_commit_from_ref(&raw_remote_anno, repo_, remote_ref.get());
  git_annotated_commitPtr remote_anno(raw_remote_anno);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  rc = git_rebase_init(rebase_in_progress_, repo_, local_anno.get(), remote_anno.get(),
                       /*onto=*/nullptr, /*opts=*/nullptr);
  // local_anno / remote_anno / refs RAII-freed at scope exit.
  if (rc != 0) {
    *rebase_in_progress_ = nullptr;
    return TranslateGitError(rc);
  }

  // Default signature for new commits created during rebase replay.
  const char *user_name =
      author_name_.empty() ? kDefaultAuthorName : author_name_.c_str();
  const char *user_email =
      author_email_.empty() ? kDefaultAuthorEmail : author_email_.c_str();
  git_signature *raw_sig = nullptr;
  rc = git_signature_now(&raw_sig, user_name, user_email);
  git_signaturePtr sig(raw_sig);
  if (rc != 0) {
    git_rebase_free(*rebase_in_progress_);  // BORROWED: keep as-is
    *rebase_in_progress_ = nullptr;
    return TranslateGitError(rc);
  }

  // Replay each rebase operation. On conflict, leave rebase_in_progress_
  // set so ResolveConflict can resume (the on-disk rebase state under
  // .git/rebase-merge is also preserved).
  return DriveRebaseLoop(repo_, rebase_in_progress_, sig.get(),
                         /*allow_eapplied=*/false,
                         /*on_conflict_result=*/VXCORE_ERR_SYNC_CONFLICT);
}

// T25: push the current branch back to origin. Refspec is built explicitly
// from git_repository_head's shorthand to avoid relying on configured
// upstream tracking. Caller must hold op_mutex_ and have repo_ open.
VxCoreError GitSyncPipeline::PushOrigin() {
  if (repo_ == nullptr) {
    return VXCORE_ERR_UNKNOWN;
  }

  git_remote *raw_remote = nullptr;
  int rc = git_remote_lookup(&raw_remote, repo_, "origin");
  git_remotePtr remote(raw_remote);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  git_push_options popts = GIT_PUSH_OPTIONS_INIT;
  // OPTION C WORKAROUND: override remote URL with PAT-embedded version for
  // this libgit2 push only. Same rationale as FetchOrigin above.
  {
    SyncCredentials cred_snapshot;
    if (creds_provider_ &&
        creds_provider_->GetCredentials(config_.remote_url, "x-access-token",
                                        &cred_snapshot) &&
        !cred_snapshot.personal_access_token.empty()) {
      const std::string embedded_url =
          MaybeEmbedPatInUrl(config_.remote_url, cred_snapshot.personal_access_token);
      if (embedded_url != config_.remote_url) {
        const int set_rc = git_remote_set_instance_url(remote.get(), embedded_url.c_str());
        if (set_rc != 0) {
          VXCORE_LOG_WARN("PushOrigin: git_remote_set_instance_url failed rc=%d, falling back to callback path", set_rc);
        }
      }
    }
  }
  auto bundle = MakeRemoteCallbacks(creds_provider_.get(), config_.remote_url, cancellation_.get());
  popts.callbacks = bundle.callbacks;

  git_reference *raw_head_ref = nullptr;
  rc = git_repository_head(&raw_head_ref, repo_);
  git_referencePtr head_ref(raw_head_ref);
  if (rc != 0) {
    return TranslateGitError(rc);
  }
  const char *branch_short = git_reference_shorthand(head_ref.get());
  if (branch_short == nullptr) {
    return VXCORE_ERR_UNKNOWN;
  }
  std::string refspec = std::string("refs/heads/") + branch_short + ":refs/heads/" + branch_short;
  head_ref.reset();

  // git_strarray takes a non-const char**; refspec.data() is valid since C++17.
  char *arr[1] = {refspec.data()};
  git_strarray refspecs = {arr, 1};

  rc = git_remote_push(remote.get(), &refspecs, &popts);
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
      author_name_.empty() ? kDefaultAuthorName : author_name_.c_str();
  const char *user_email =
      author_email_.empty() ? kDefaultAuthorEmail : author_email_.c_str();
  git_signature *raw_sig = nullptr;
  int rc = git_signature_now(&raw_sig, user_name, user_email);
  git_signaturePtr sig(raw_sig);
  if (rc != 0) {
    return TranslateGitError(rc);
  }

  // Commit the just-resolved operation. GIT_EAPPLIED means the resolution
  // produced an empty commit (e.g. KeepRemote on an exact match) — treat as
  // success and continue.
  git_oid commit_oid;
  rc = git_rebase_commit(&commit_oid, *rebase_in_progress_, /*author=*/sig.get(),
                         /*committer=*/sig.get(), /*message_encoding=*/nullptr,
                         /*message=*/nullptr);
  if (rc != 0 && rc != GIT_EAPPLIED) {
    return TranslateGitError(rc);
  }
  git_error_clear();

  return DriveRebaseLoop(repo_, rebase_in_progress_, sig.get(),
                         /*allow_eapplied=*/true,
                         /*on_conflict_result=*/VXCORE_OK);
}

}  // namespace vxcore
