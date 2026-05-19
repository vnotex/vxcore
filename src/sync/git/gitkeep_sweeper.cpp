#include "sync/git/gitkeep_sweeper.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "sync/git/git_defaults.h"  // For WriteIfMissing
#include "utils/file_utils.h"       // For PathFilename, PathToUtf8, PathFromUtf8
#include "utils/logger.h"           // For VXCORE_LOG_WARN
#include "utils/string_utils.h"     // For MatchesPatterns

namespace vxcore {

namespace {

// ---------- gitkeep injection helpers ----------

static constexpr const char kGitkeepFilename[] = ".gitkeep";

// Root-relative POSIX prefixes that are always skipped.
static const std::vector<std::string> &GitkeepSkipPrefixes() {
  static const std::vector<std::string> prefixes = {
    "vx_notebook",
    "vx_recycle_bin",
  };
  return prefixes;
}

// True if rel_posix is the notebook root ("" or "."), begins with any
// skip-prefix at the ROOT only (e.g. "vx_notebook" or "vx_notebook/..."
// but NOT "MyFolder/vx_notebook/..."), or starts with "." or "_v_" at any
// component (component-prefix check, not substring).
bool ShouldSkipForGitkeep(const std::string &rel_posix) {
  if (rel_posix.empty() || rel_posix == ".") {
    return true;
  }

  // Split on '/' and check components
  std::vector<std::string> components;
  size_t start = 0;
  for (size_t i = 0; i <= rel_posix.size(); ++i) {
    if (i == rel_posix.size() || rel_posix[i] == '/') {
      if (i > start) {
        components.push_back(rel_posix.substr(start, i - start));
      }
      start = i + 1;
    }
  }

  for (size_t idx = 0; idx < components.size(); ++idx) {
    const auto &comp = components[idx];

    // First component only: check skip-prefixes
    if (idx == 0) {
      for (const auto &prefix : GitkeepSkipPrefixes()) {
        if (comp == prefix) {
          return true;
        }
      }
    }

    // Every component: check for "." or "_v_" prefix
    if (comp.rfind(".", 0) == 0 || comp.rfind("_v_", 0) == 0) {
      return true;
    }
  }

  return false;
}

// True iff the file at gitkeep_path exists, is a regular file, and is
// exactly 0 bytes (i.e., VNote-owned). Errors are reported via ec; on
// error returns false (do not treat as owned).
bool IsOwnedMarker(const std::filesystem::path &gitkeep_path, std::error_code &ec) {
  auto s = std::filesystem::status(gitkeep_path, ec);
  if (ec) {
    ec.clear();
    return false;
  }
  if (!std::filesystem::is_regular_file(s)) {
    return false;
  }
  auto sz = std::filesystem::file_size(gitkeep_path, ec);
  if (ec) {
    return false;
  }
  return sz == 0;
}

// True iff dir's immediate children contain ZERO entries that are:
//   (a) regular files not matching exclude_globs and not named ".gitkeep", OR
//   (b) sub-directories whose POSIX-relative path (rel_posix + "/" + name) is
//       NOT skipped per ShouldSkipForGitkeep.
// Errors during iteration captured into ec; on error returns false.
bool IsEffectivelyEmpty(const std::filesystem::path &dir,
                        const std::string &rel_posix,
                        const std::vector<std::string> &exclude_globs,
                        std::error_code &ec) {
  std::filesystem::directory_iterator it(dir, ec);
  if (ec) {
    ec.clear();
    return false;
  }

  for (const auto &entry : it) {
    std::error_code local_ec;

    if (entry.is_regular_file(local_ec)) {
      const auto entry_name_utf8 = PathFilename(PathToUtf8(entry.path()));
      if (entry_name_utf8 == kGitkeepFilename) {
        continue;  // never counts
      }
      if (MatchesPatterns(entry_name_utf8, exclude_globs)) {
        continue;  // excluded by pattern
      }
      return false;  // a real file that counts
    }

    if (entry.is_directory(local_ec)) {
      const auto entry_name_utf8 = PathFilename(PathToUtf8(entry.path()));
      const std::string child_rel = rel_posix.empty()
          ? entry_name_utf8
          : rel_posix + "/" + entry_name_utf8;
      if (ShouldSkipForGitkeep(child_rel)) {
        continue;
      }
      return false;  // a real subdirectory that counts
    }

    // Symlink or special file — skip it
  }

  return true;  // folder IS effectively empty
}

}  // namespace

// T6: Two-pass sweep to keep empty/non-empty directory state aligned with
// VNote-owned .gitkeep markers. Pass 1 walks the workdir (recursive_directory_
// iterator with skip_permission_denied) and collects two disjoint candidate
// lists; Pass 2 performs the file I/O. Mutating during traversal can invalidate
// the iterator on some platforms, so we strictly collect-then-mutate. All
// std::filesystem calls use the std::error_code overload and the loop is wrapped
// in try/catch so no exceptions escape — failure to sweep is non-fatal.
void EnsureGitkeepFiles(const std::string &root_folder, const SyncConfig &config) {
  // root_folder is the notebook root (= libgit2 workdir).
  const std::filesystem::path root = PathFromUtf8(root_folder);

  // Pass 1: collect candidates.
  std::vector<std::filesystem::path> dirs_to_seed;     // need .gitkeep created
  std::vector<std::filesystem::path> dirs_to_cleanup;  // need .gitkeep removed
  std::error_code ec;
  try {
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
      if (ec) {
        VXCORE_LOG_WARN("EnsureGitkeepFiles: iterator error: %s", ec.message().c_str());
        ec.clear();
        continue;
      }
      if (!it->is_directory(ec)) {
        ec.clear();
        continue;
      }
      const std::filesystem::path abs = it->path();
      // Compute POSIX-style relative path (forward slashes).
      const std::filesystem::path rel = std::filesystem::relative(abs, root, ec);
      ec.clear();
      std::string rel_posix = PathToUtf8(rel);
      std::replace(rel_posix.begin(), rel_posix.end(), '\\', '/');
      if (ShouldSkipForGitkeep(rel_posix)) {
        it.disable_recursion_pending();  // do not descend into skipped dirs
        continue;
      }
      const std::filesystem::path marker = abs / kGitkeepFilename;
      const bool marker_exists = std::filesystem::exists(marker, ec);
      ec.clear();
      if (IsEffectivelyEmpty(abs, rel_posix, config.exclude_paths, ec)) {
        ec.clear();
        if (!marker_exists) {
          dirs_to_seed.push_back(abs);
        }
        // else: already has marker — leave alone (idempotent)
      } else {
        ec.clear();
        if (marker_exists && IsOwnedMarker(marker, ec)) {
          ec.clear();
          dirs_to_cleanup.push_back(abs);
        }
      }
    }
  } catch (const std::exception &e) {
    VXCORE_LOG_WARN("EnsureGitkeepFiles: traversal aborted: %s", e.what());
    return;
  }

  // Pass 2: mutate.
  for (const auto &dir : dirs_to_seed) {
    const std::filesystem::path marker = dir / kGitkeepFilename;
    if (!WriteIfMissing(PathToUtf8(marker), "")) {
      VXCORE_LOG_WARN("EnsureGitkeepFiles: failed to write %s",
                      PathToUtf8(marker).c_str());
    }
  }
  for (const auto &dir : dirs_to_cleanup) {
    const std::filesystem::path marker = dir / kGitkeepFilename;
    std::error_code rm_ec;
    std::filesystem::remove(marker, rm_ec);
    if (rm_ec) {
      VXCORE_LOG_WARN("EnsureGitkeepFiles: failed to remove %s: %s",
                      PathToUtf8(marker).c_str(), rm_ec.message().c_str());
    }
  }
}

}  // namespace vxcore
