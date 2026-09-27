#include "sync_encryption_guard.h"

#include <git2.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string_view>

#include "core/notebook.h"
#include "sync/git/git_conflict_resolver.h"
#include "sync/git/libgit2_init.h"
#include "sync/sync_json_keys.h"
#include "utils/file_utils.h"
#include "vxcore/notebook_json_keys.h"

namespace vxcore {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr const char *kKeyPath = "vx_notebook/encryption.vne";

struct InvalidState {};
void Require(bool condition) {
  if (!condition) throw InvalidState{};
}

bool Exists(const fs::path &path) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) return false;
  Require(!error);
  if (!fs::exists(status)) return false;
  Require(CheckReparsePoint(PathToUtf8(path)) == ReparseState::kNo);
  return true;
}

bool Directory(const fs::path &path) {
  if (!Exists(path)) return false;
  Require(fs::is_directory(path));
  return true;
}

bool SafePath(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.find_first_of("\\:\0", 0, 3) != std::string::npos)
    return false;
  size_t start = 0;
  while (start < path.size()) {
    const auto end = path.find('/', start);
    const auto part = path.substr(start, end == std::string::npos ? end : end - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string::npos) return true;
    start = end + 1;
  }
  return false;
}

bool KeyPath(std::string_view path) {
  constexpr std::string_view key(kKeyPath);
  return path.size() == key.size() &&
         std::equal(path.begin(), path.end(), key.begin(), [](char actual, char expected) {
           if (actual >= 'A' && actual <= 'Z') actual += 'a' - 'A';
           return actual == expected;
         });
}

bool Hash(const Json &value) {
  if (!value.is_string()) return false;
  const auto &text = value.get_ref<const std::string &>();
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

Json ReadState(const fs::path &path) {
  Require(fs::is_regular_file(path) && fs::file_size(path) <= 128 * 1024 * 1024);
  std::ifstream input(path, std::ios::binary);
  Require(input.is_open());
  auto json = Json::parse(input);
  Require(!input.bad() && json.is_object() && json.at("version").is_number_integer() &&
          json.at("version") == 1 && json.at(kJsonKeyNotebookId).is_string() &&
          NotebookEncryption::IsCanonicalUuid(json.at(kJsonKeyNotebookId).get<std::string>()) &&
          json.at(kJsonKeyRemoteUrl).is_string() &&
          !json.at(kJsonKeyRemoteUrl).get_ref<const std::string &>().empty() &&
          Hash(json.at("usernameHash")));
  return json;
}

struct KeyState {
  bool conflict = false;
  bool pending = false;
  bool replace_local = false;
  bool any_conflict = false;
  bool any_pending = false;
};

KeyState InspectWebDav(const fs::path &directory) {
  KeyState result;
  if (!Directory(directory)) return result;
  for (const auto &entry : fs::directory_iterator(directory)) {
    const auto name = PathToUtf8(entry.path().filename());
    Require(name == "state.json" || name == "pending.json" || name == "snapshots" ||
            name == "probe" || name == "retired");
    Require(Exists(entry.path()));
  }
  Json state;
  if (Exists(directory / "state.json")) {
    state = ReadState(directory / "state.json");
    Require(state.at("entries").is_object() && state.at("conflicts").is_object());
    for (auto it = state.at("conflicts").begin(); it != state.at("conflicts").end(); ++it) {
      Require(SafePath(it.key()) && it.value().is_object());
      result.any_conflict = true;
      if (KeyPath(it.key())) {
        const auto &conflict = it.value();
        for (const char *field : {"localSha256", "remoteSha256"})
          Require(conflict.at(field).is_null() || Hash(conflict.at(field)));
        Require(conflict.at(kJsonKeyIsBinary).is_boolean() &&
                conflict.at(kJsonKeyCanKeepBoth) == false);
        if (conflict.contains("resolution")) {
          Require(conflict.at("resolution") == "keep_local" ||
                  conflict.at("resolution") == "keep_remote");
        }
        // A recorded choice is still unresolved until the applying round trip confirms it.
        result.conflict = true;
      }
    }
  }
  if (Exists(directory / "pending.json")) {
    const auto pending = ReadState(directory / "pending.json");
    Require(!state.is_null() && pending.at(kJsonKeyNotebookId) == state.at(kJsonKeyNotebookId) &&
            pending.at(kJsonKeyRemoteUrl) == state.at(kJsonKeyRemoteUrl) &&
            pending.at("usernameHash") == state.at("usernameHash") &&
            pending.at("operationId").is_string() &&
            NotebookEncryption::IsCanonicalUuid(pending.at("operationId").get<std::string>()) &&
            pending.at("operations").is_array() && pending.at("operations").size() <= 500000);
    for (const auto &op : pending.at("operations")) {
      Require(op.is_object() && op.at("path").is_string());
      const auto &path = op.at("path").get_ref<const std::string &>();
      Require(SafePath(path));
      const auto &action = op.at("action").get_ref<const std::string &>();
      const auto &stage = op.at("stage").get_ref<const std::string &>();
      Require(action == "upsertLocal" || action == "upsertRemote" || action == "deleteLocal" ||
              action == "deleteRemote" || action == "mkdirLocal" || action == "mkdirRemote" ||
              action == "removeLocalCollection");
      Require(stage == "prepared" || stage == "remoteConfirmed" || stage == "localConfirmed" ||
              stage == "baselineConfirmed");
      result.any_pending = true;
      if (!KeyPath(path)) continue;
      Require(op.at("kind") == "file" && action != "mkdirLocal" && action != "mkdirRemote" &&
              action != "removeLocalCollection");
      for (const char *field : {"expectedLocalSha256", "oldSha256", "newSha256"})
        Require(op.at(field).is_null() || Hash(op.at(field)));
      Require((action != "upsertLocal" && action != "upsertRemote") || Hash(op.at("newSha256")));
      result.pending = true;
      result.replace_local =
          result.replace_local || action == "upsertLocal" || action == "deleteLocal";
    }
  }
  return result;
}

VxCoreError CheckWebDav(const fs::path &directory, bool all_paths) {
  const auto unresolved = [all_paths](const KeyState &state) {
    return all_paths ? state.any_conflict || state.any_pending : state.conflict || state.pending;
  };
  if (unresolved(InspectWebDav(directory))) return VXCORE_ERR_SYNC_CONFLICT;
  if (all_paths && unresolved(InspectWebDav(directory / "probe"))) return VXCORE_ERR_SYNC_CONFLICT;
  // Completed archives are historical data, not an active binding to retire.
  if (all_paths) return VXCORE_OK;
  const auto retired = directory / "retired";
  if (Directory(retired)) {
    for (const auto &entry : fs::directory_iterator(retired)) {
      Require(NotebookEncryption::IsCanonicalUuid(PathToUtf8(entry.path().filename())));
      if (unresolved(InspectWebDav(entry.path()))) return VXCORE_ERR_SYNC_CONFLICT;
    }
  }
  return VXCORE_OK;
}
}  // namespace

VxCoreError CheckNotebookEncryptionSyncState(const std::string &metadata_folder) {
  try {
    const auto metadata = PathFromUtf8(metadata_folder);
    Require(Directory(metadata));
    const auto sync = metadata / "vx_sync";
    if (!Directory(sync)) return VXCORE_OK;
    bool git_owned = false;
    for (const auto &entry : fs::directory_iterator(sync)) {
      Require(Exists(entry.path()));
      if (entry.path().filename() != "webdav") git_owned = true;
    }
    // Git uses vx_sync itself as its repository. A WebDAV child is not Git evidence;
    // any other unexplained/non-readable content still fails closed in the Git inspector.
    if (git_owned) {
      const auto error = GitConflictResolver::CheckEncryptionKeyConflict(PathToUtf8(sync));
      if (error != VXCORE_OK) return error;
    }
    return CheckWebDav(sync / "webdav", false);
  } catch (...) {
    return VXCORE_ERR_ENCRYPTION_SYNC_STATE;
  }
}

VxCoreError CheckNotebookSyncReconfiguration(const std::string &metadata_folder) {
  try {
    const auto metadata = PathFromUtf8(metadata_folder);
    Require(Directory(metadata));
    const auto sync = metadata / "vx_sync";
    if (!Directory(sync)) return VXCORE_OK;
    bool git_owned = false;
    for (const auto &entry : fs::directory_iterator(sync)) {
      Require(Exists(entry.path()));
      if (entry.path().filename() != "webdav") git_owned = true;
    }
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
    return CheckWebDav(sync / "webdav", true);
  } catch (...) {
    return VXCORE_ERR_INVALID_STATE;
  }
}

VxCoreError PrepareNotebookEncryptionSyncApply(Notebook &notebook,
                                               const std::vector<std::string> &protected_paths) {
  try {
    const auto metadata = PathFromUtf8(notebook.GetMetadataFolder());
    Require(Directory(metadata));
    const auto sync = metadata / "vx_sync";
    if (!Directory(sync)) return VXCORE_OK;
    const auto state = InspectWebDav(sync / "webdav");
    if (!state.replace_local) return VXCORE_OK;
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
