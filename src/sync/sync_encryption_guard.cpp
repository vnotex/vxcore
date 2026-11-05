#include "sync_encryption_guard.h"

#include <git2.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string_view>

#include "core/notebook.h"
#include "sync/git/git_conflict_resolver.h"
#include "sync/git/libgit2_init.h"
#include "sync/jianguoyun/jianguoyun_state.h"
#include "sync/sync_provider_retirement.h"
#include "utils/file_utils.h"

namespace vxcore {
namespace {
namespace fs = std::filesystem;
constexpr const char *kKeyPath = "vx_notebook/encryption.vne";

struct InvalidState {};
void Require(bool condition) {
  if (!condition) throw InvalidState{};
}

fs::path ResolveSyncMetadata(const std::string &metadata_folder) {
  const auto metadata = PathFromUtf8(metadata_folder);
  // Validate the original spelling before resolving anything across the trust boundary.
  const NativePathArgument original(metadata, FileAccess::kPrivate);
  const auto notebook = metadata.parent_path();
  const auto ancestor = notebook.parent_path();
  Require(!metadata.filename().empty() && !notebook.filename().empty() && !ancestor.empty() &&
          ancestor != notebook);
  // Only ancestors ABOVE the notebook may be aliases. Never decompose native spelling.
  const NativePathArgument native_ancestor(ancestor, FileAccess::kPrivate);
  auto physical_parent = fs::canonical(native_ancestor.c_str());
#ifdef _WIN32
  const auto &native = physical_parent.native();
  if (native.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
    physical_parent = fs::path(L"\\\\" + native.substr(8));
  } else if (native.compare(0, 4, L"\\\\?\\") == 0) {
    physical_parent = fs::path(native.substr(4));
  }
#endif
  return physical_parent / notebook.filename() / metadata.filename();
}

bool Exists(const fs::path &path, FileAccess access = FileAccess::kDefault) {
  if (access == FileAccess::kPrivate) return private_io::Exists(path);
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) return false;
  Require(!error);
  if (!fs::exists(status)) return false;
  Require(CheckReparsePoint(PathToUtf8(path)) == ReparseState::kNo);
  return true;
}

bool Directory(const fs::path &path, FileAccess access = FileAccess::kDefault) {
  if (!Exists(path, access)) return false;
  Require(access == FileAccess::kPrivate ? private_io::IsDirectory(path) : fs::is_directory(path));
  return true;
}

bool KeyPath(std::string_view path) {
  constexpr std::string_view key(kKeyPath);
  return path.size() == key.size() &&
         std::equal(path.begin(), path.end(), key.begin(), [](char actual, char expected) {
           if (actual >= 'A' && actual <= 'Z') actual += 'a' - 'A';
           return actual == expected;
         });
}

VxCoreError CheckWebDav(const fs::path &directory, bool all_paths) {
  WebDavStorageStatus state;
  const auto error = InspectWebDavStorage(directory, state);
  if (error != VXCORE_OK) return all_paths ? error : VXCORE_ERR_ENCRYPTION_SYNC_STATE;
  const bool unresolved = all_paths ? state.any_conflict || state.any_pending
                                    : state.encryption_conflict || state.encryption_pending;
  return unresolved ? VXCORE_ERR_SYNC_CONFLICT : VXCORE_OK;
}
VxCoreError CheckJianguoyun(const fs::path &directory, bool all_paths) {
  if (!Directory(directory)) return VXCORE_OK;
  const auto check = [all_paths](const fs::path &path) {
    jianguoyun::RecoveryStatus state;
    const auto error = jianguoyun::InspectRecovery(path, state);
    if (error != VXCORE_OK) return error;
    const bool unresolved = all_paths ? state.any_conflict || state.any_pending
                                      : state.encryption_conflict || state.encryption_pending;
    return unresolved ? VXCORE_ERR_SYNC_CONFLICT : VXCORE_OK;
  };
  const auto error = check(directory);
  if (error != VXCORE_OK || all_paths) return error;
  // Retired histories can still contain key-envelope recovery that protects reads.
  const auto retired = directory / "retired";
  if (Directory(retired)) {
    for (const auto &entry : fs::directory_iterator(retired)) {
      Require(EncryptedNote::IsCanonicalUuid(PathToUtf8(entry.path().filename())));
      const auto archived = check(entry.path());
      if (archived != VXCORE_OK) return archived;
    }
  }
  return VXCORE_OK;
}
}  // namespace

VxCoreError CheckNotebookEncryptionSyncState(const std::string &metadata_folder) {
  try {
    const auto metadata = ResolveSyncMetadata(metadata_folder);
    Require(Directory(metadata, FileAccess::kPrivate));
    const auto sync = metadata / "vx_sync";
    if (!Directory(sync, FileAccess::kPrivate)) return VXCORE_OK;
    bool git_owned = false;
    private_io::VisitDirectory(sync, [&](const fs::path &entry) {
      Require(Exists(entry, FileAccess::kPrivate));
      if (entry.filename() != "webdav" && entry.filename() != "jianguoyun")
        git_owned = true;
    });
    // Git uses vx_sync itself as its repository. DAV provider directories are not Git evidence;
    // any other unexplained/non-readable content still fails closed in the Git inspector.
    if (git_owned) {
      const auto error = GitConflictResolver::CheckEncryptionKeyConflict(PathToUtf8(sync));
      if (error != VXCORE_OK) return error;
    }
    const auto webdav_error = CheckWebDav(sync / "webdav", false);
    return webdav_error == VXCORE_OK ? CheckJianguoyun(sync / "jianguoyun", false) : webdav_error;
  } catch (...) {
    return VXCORE_ERR_ENCRYPTION_SYNC_STATE;
  }
}

VxCoreError CheckNotebookSyncReconfiguration(const std::string &metadata_folder) {
  try {
    const auto metadata = ResolveSyncMetadata(metadata_folder);
    Require(Directory(metadata, FileAccess::kPrivate));
    const auto sync = metadata / "vx_sync";
    if (!Directory(sync, FileAccess::kPrivate)) return VXCORE_OK;
    bool git_owned = false;
    private_io::VisitDirectory(sync, [&](const fs::path &entry) {
      Require(Exists(entry, FileAccess::kPrivate));
      if (entry.filename() != "webdav" && entry.filename() != "jianguoyun")
        git_owned = true;
    });
    if (git_owned) {
      LibGit2Init init;
      if (!LibGit2Init::ok()) return VXCORE_ERR_GIT_INIT_FAILED;
      git_repository *raw_repo = nullptr;
      if (git_repository_open_ext(&raw_repo, PathToUtf8(sync).c_str(),
                                  GIT_REPOSITORY_OPEN_NO_SEARCH | GIT_REPOSITORY_OPEN_BARE,
                                  nullptr) != 0)
        return VXCORE_ERR_INVALID_STATE;
      std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(raw_repo,
                                                                           git_repository_free);
      git_index *raw_index = nullptr;
      if (git_repository_index(&raw_index, repo.get()) != 0) return VXCORE_ERR_INVALID_STATE;
      std::unique_ptr<git_index, decltype(&git_index_free)> index(raw_index, git_index_free);
      if (git_index_has_conflicts(index.get())) return VXCORE_ERR_SYNC_CONFLICT;
      if (git_repository_state(repo.get()) != GIT_REPOSITORY_STATE_NONE)
        return VXCORE_ERR_SYNC_IN_PROGRESS;
    }
    const auto webdav_error = CheckWebDav(sync / "webdav", true);
    return webdav_error == VXCORE_OK ? CheckJianguoyun(sync / "jianguoyun", true) : webdav_error;
  } catch (...) {
    return VXCORE_ERR_INVALID_STATE;
  }
}

VxCoreError PrepareNotebookEncryptionSyncApply(Notebook &notebook,
                                               const std::vector<std::string> &protected_paths) {
  try {
    const auto metadata = ResolveSyncMetadata(notebook.GetMetadataFolder());
    Require(Directory(metadata, FileAccess::kPrivate));
    const auto sync = metadata / "vx_sync";
    if (!Directory(sync, FileAccess::kPrivate)) return VXCORE_OK;
    WebDavStorageStatus state;
    Require(InspectWebDavStorage(sync / "webdav", state) == VXCORE_OK);
    jianguoyun::RecoveryStatus managed;
    Require(jianguoyun::InspectRecovery(sync / "jianguoyun", managed) == VXCORE_OK);
    if (!state.replaces_encryption && !managed.replaces_encryption) return VXCORE_OK;
    for (const auto &path : protected_paths) {
      const bool encrypted = path.size() >= 4 && std::equal(path.end() - 4, path.end(), ".vne",
                                                            [](char actual, char expected) {
                                                              if (actual >= 'A' && actual <= 'Z')
                                                                actual += 'a' - 'A';
                                                              return actual == expected;
                                                            });
      if (KeyPath(path) || encrypted) {
        return VXCORE_ERR_SYNC_IN_PROGRESS;
      }
    }
    NotebookEncryption *encryption = nullptr;
    auto error = notebook.EnsureEncryption(encryption);
    if (error != VXCORE_OK) return error;
    error = encryption->LockNotebookKey();
    return error == VXCORE_ERR_INVALID_STATE ? VXCORE_ERR_SYNC_IN_PROGRESS : error;
  } catch (...) {
    return VXCORE_ERR_ENCRYPTION_SYNC_STATE;
  }
}

}  // namespace vxcore
