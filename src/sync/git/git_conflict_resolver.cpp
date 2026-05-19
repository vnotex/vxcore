#include "sync/git/git_conflict_resolver.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include "sync/git/git_error_translator.h"
#include "sync/git/git_sync_pipeline.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

GitConflictResolver::GitConflictResolver(git_repository *repo, const std::string &root_folder)
    : repo_(repo), root_folder_(root_folder) {}

VxCoreError GitConflictResolver::GetConflicts(std::vector<SyncConflictInfo> &out_conflicts) {
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

VxCoreError GitConflictResolver::ResolveConflict(const std::string &path,
                                                SyncConflictResolution resolution,
                                                GitSyncPipeline &pipeline) {
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

  return pipeline.ContinueRebaseAfterResolution();
}

}  // namespace vxcore
