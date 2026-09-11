#include "node_transfer.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

#include <vxcore/notebook_json_keys.h>

#include "bundled_folder_manager.h"
#include "bundled_notebook.h"
#include "config_manager.h"
#include "content_processor/content_processor.h"
#include "event_names.h"
#include "metadata_store.h"
#include "notebook_manager.h"
#include "sync/git/git_conflict_resolver.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

namespace fs = std::filesystem;

namespace {

constexpr const char *kTransferDirName = "vx_transfer";
constexpr const char *kManifestName = "manifest.json";
constexpr const char *kJournalName = "journal.json";
constexpr const char *kContentName = "content";
constexpr const char *kMetadataName = "metadata";
constexpr const char *kAssetsName = "assets";
constexpr const char *kKindDestination = "destination";
constexpr const char *kKindSourceRemoval = "sourceRemoval";
constexpr const char *kPhaseInit = "init";
constexpr const char *kPhasePublished = "published";
constexpr const char *kPhaseDb = "db";
constexpr const char *kPhaseCommitted = "committed";
constexpr const char *kPhaseQuarantined = "quarantined";
constexpr const char *kPhaseSourceCommitted = "sourceCommitted";

struct IndexedFile {
  std::string relative_path;
  FileRecord source_record;
  FileRecord destination_record;
  std::string source_assets_root;
  std::string staged_assets_root;
  bool has_backup = false;
};

struct IndexedFolder {
  std::string relative_path;
  FolderConfig source_config;
  FolderConfig destination_config;
};

struct ProtectedTransferKeys {
  std::shared_ptr<const NotebookEncryption::Key> source;
  std::shared_ptr<const NotebookEncryption::Key> destination;
  NotebookEncryption::KeyEnvelope source_envelope;
  NotebookEncryption::KeyEnvelope destination_envelope;
};

struct PreparedNodeTransferImpl : PreparedNodeTransfer {
  NotebookManager *notebook_manager = nullptr;
  Notebook *source_override = nullptr;
  Notebook *destination_override = nullptr;
  std::unique_ptr<Notebook> bundle_source;
  std::string source_notebook_id;
  std::string source_relative_path;
  std::string destination_notebook_id;
  std::string destination_folder_path;
  std::string operation;
  bool preserve_timestamps = false;
  bool create_missing_tags = true;
  bool test_fail_post_commit_verification = false;
  bool test_fail_commit_journal = false;
  bool test_fail_result_before_commit = false;
  bool test_fail_result_after_commit = false;
  bool test_fail_source_removal = false;
  bool test_fail_source_rollback_content = false;
  bool test_fail_source_rollback_parent = false;
  bool test_throw_after_source_quarantine = false;
  bool test_destination_publication_race = false;
  bool is_folder = false;
  std::string source_name;
  std::string destination_node_id;
  std::string staging_dir;
  std::string source_recovery_id;
  std::string source_fingerprint;
  std::vector<IndexedFile> files;
  std::vector<IndexedFolder> folders;
  std::vector<std::string> tag_paths;
  std::unique_ptr<ProtectedTransferKeys> protected_keys;
};

// A bundle is an immutable source, not a session notebook or an indexing job.
// It deliberately has no metadata database and cannot modify its backing tree.
class BundleNotebook final : public Notebook {
 public:
  BundleNotebook(const std::string &root, NotebookConfig config)
      : Notebook("", root, NotebookType::Bundled) {
    config_ = std::move(config);
    folder_manager_ = std::make_unique<BundledFolderManager>(this);
    SetReadOnly(true);
  }
  std::string GetMetadataFolder() const override {
    return ConcatenatePaths(root_folder_, "vx_notebook");
  }
  std::string GetConfigPath() const override {
    return ConcatenatePaths(GetMetadataFolder(), "config.json");
  }
  VxCoreError UpdateConfig(const NotebookConfig &) override { return VXCORE_ERR_READ_ONLY; }
  VxCoreError RebuildCache() override { return VXCORE_ERR_READ_ONLY; }
  std::string GetRecycleBinPath() const override { return {}; }
  VxCoreError EmptyRecycleBin() override { return VXCORE_ERR_READ_ONLY; }
};

VxCoreError ValidateTransferEnvelope(Notebook *notebook,
                                     const NotebookEncryption::KeyEnvelope &expected) {
  auto error = GitConflictResolver::CheckEncryptionKeyConflict(
      ConcatenatePaths(notebook->GetMetadataFolder(), "vx_sync"));
  if (error != VXCORE_OK) return error;
  NotebookEncryption::KeyEnvelope current;
  error = NotebookEncryption::ReadKeyEnvelope(
      PathFromUtf8(notebook->GetMetadataFolder()) / "encryption.vne", current);
  if (error != VXCORE_OK) return error;
  std::string current_bytes, expected_bytes;
  error = NotebookEncryption::EncodeKeyEnvelope(current, current_bytes);
  if (error == VXCORE_OK) error = NotebookEncryption::EncodeKeyEnvelope(expected, expected_bytes);
  return error != VXCORE_OK ? error
      : (current_bytes == expected_bytes ? VXCORE_OK : VXCORE_ERR_ENCRYPTION_LOCKED);
}

uint64_t FnvAppend(uint64_t hash, const void *data, size_t size) {
  const auto *bytes = static_cast<const unsigned char *>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

uint64_t FnvAppend(uint64_t hash, const std::string &value) {
  hash = FnvAppend(hash, value.data(), value.size());
  const unsigned char separator = 0;
  return FnvAppend(hash, &separator, 1);
}

std::string HashString(uint64_t hash) {
  char output[17] = {};
  std::snprintf(output, sizeof(output), "%016llx", static_cast<unsigned long long>(hash));
  return output;
}

bool WriteDurable(const fs::path &path, const std::string &data) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    return false;
  }
#ifdef _WIN32
  FILE *file = _wfopen(path.c_str(), L"wb");
#else
  FILE *file = std::fopen(path.c_str(), "wb");
#endif
  if (!file) {
    return false;
  }
  bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), file) == data.size();
  if (ok) {
    ok = std::fflush(file) == 0;
  }
  if (ok) {
#ifdef _WIN32
    ok = _commit(_fileno(file)) == 0;
#else
    ok = ::fsync(fileno(file)) == 0;
#endif
  }
  std::fclose(file);
  return ok;
}

bool ReplaceDurable(const fs::path &path, const std::string &data) {
  fs::path temporary = path;
  temporary += PathFromUtf8(".transfer.tmp");
  if (!WriteDurable(temporary, data)) {
    return false;
  }
#ifdef _WIN32
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::error_code ec;
    fs::remove(temporary, ec);
    return false;
  }
#else
  std::error_code ec;
  fs::rename(temporary, path, ec);
  if (ec) {
    fs::remove(temporary, ec);
    return false;
  }
#endif
  return true;
}

void RemoveQuietly(const fs::path &path) {
  std::error_code ec;
  fs::remove_all(path, ec);
  if (ec) {
    VXCORE_LOG_WARN("NodeTransfer: failed to remove %s: %s", PathToUtf8(path).c_str(),
                    ec.message().c_str());
  }
}

bool PathExistsChecked(const fs::path &path, bool &out_exists) {
  std::error_code ec;
  const fs::file_status status = fs::symlink_status(path, ec);
  if (ec == std::errc::no_such_file_or_directory) {
    out_exists = false;
    return true;
  }
  if (ec) {
    return false;
  }
  out_exists = status.type() != fs::file_type::not_found;
  return true;
}

bool RemovePathChecked(const fs::path &path) {
  bool exists = false;
  if (path.empty() || !PathExistsChecked(path, exists)) {
    return false;
  }
  if (!exists) {
    return true;
  }
  std::error_code ec;
  fs::remove_all(path, ec);
  if (ec) {
    VXCORE_LOG_ERROR("NodeTransfer: failed to remove %s: %s", PathToUtf8(path).c_str(),
                     ec.message().c_str());
    return false;
  }
  return PathExistsChecked(path, exists) && !exists;
}

bool CleanupJournalDir(const fs::path &directory) {
  bool exists = false;
  if (!PathExistsChecked(directory, exists)) {
    return false;
  }
  if (!exists) {
    return true;
  }
  std::error_code ec;
  std::vector<fs::path> children;
  for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().filename() != PathFromUtf8(kJournalName)) {
      children.push_back(it->path());
    }
  }
  if (ec) {
    return false;
  }
  for (const auto &child : children) {
    if (!RemovePathChecked(child)) {
      return false;
    }
  }
  const fs::path journal_path = directory / kJournalName;
  if (!RemovePathChecked(journal_path)) {
    return false;
  }
  fs::remove(directory, ec);
  return !ec || ec == std::errc::no_such_file_or_directory;
}

bool RenamePath(const fs::path &from, const fs::path &to) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  if (ec) {
    return false;
  }
  fs::rename(from, to, ec);
  return !ec;
}

bool RestoreRenamedPath(const fs::path &quarantine, const fs::path &source) {
  bool quarantine_exists = false;
  bool source_exists = false;
  if (!PathExistsChecked(quarantine, quarantine_exists) ||
      !PathExistsChecked(source, source_exists)) {
    return false;
  }
  if (quarantine_exists) {
    if (source_exists || !RenamePath(quarantine, source)) {
      return false;
    }
    return PathExistsChecked(quarantine, quarantine_exists) && !quarantine_exists &&
           PathExistsChecked(source, source_exists) && source_exists;
  }
  return source_exists;
}

bool IsSafeRelativePath(const std::string &path, bool allow_root) {
  if (path.empty()) {
    return allow_root;
  }
  const fs::path value = PathFromUtf8(path);
  if (value.is_absolute()) {
    return false;
  }
  const fs::path clean = value.lexically_normal();
  if (clean == ".") {
    return allow_root;
  }
  for (const auto &part : clean) {
    const std::string component = PathToUtf8(part);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
  }
  return true;
}

VxCoreError CreateNameComparerProbe(const fs::path &parent, fs::path &out_probe) {
  out_probe.clear();
  for (int attempt = 0; attempt < 8; ++attempt) {
    const fs::path probe = parent / PathFromUtf8(".vx-transfer-name-probe-" + GenerateUUID());
    std::error_code ec;
    if (!fs::create_directory(probe, ec)) {
      if (ec) {
        return VXCORE_ERR_IO;
      }
      continue;
    }
#ifdef _WIN32
    if (!SetFileAttributesW(probe.c_str(), FILE_ATTRIBUTE_HIDDEN)) {
      fs::remove(probe, ec);
      return VXCORE_ERR_IO;
    }
#endif
    out_probe = probe;
    return VXCORE_OK;
  }
  return VXCORE_ERR_ALREADY_EXISTS;
}

struct NameComparerProbe {
  ~NameComparerProbe() { RemoveQuietly(path); }
  fs::path path;
};

enum class PublishResult { Published, Collision, Error };

PublishResult PublishPathNoReplace(const fs::path &from, const fs::path &to) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  if (ec) {
    return PublishResult::Error;
  }
#ifdef _WIN32
  if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
    return PublishResult::Published;
  }
  const DWORD error = GetLastError();
  return error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS ? PublishResult::Collision
                                                                     : PublishResult::Error;
#elif defined(__linux__)
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#ifdef SYS_renameat2
  if (::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) ==
      0) {
    return PublishResult::Published;
  }
  return errno == EEXIST || errno == ENOTEMPTY ? PublishResult::Collision : PublishResult::Error;
#else
  (void)from;
  (void)to;
  return PublishResult::Error;
#endif
#elif defined(__APPLE__)
  if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) {
    return PublishResult::Published;
  }
  return errno == EEXIST || errno == ENOTEMPTY ? PublishResult::Collision : PublishResult::Error;
#else
  (void)from;
  (void)to;
  return PublishResult::Error;
#endif
}

bool HasUnsafeComponent(const fs::path &root, const fs::path &path) {
  std::error_code ec;
  fs::path current = root;
  fs::path relative = fs::relative(path, root, ec);
  if (ec || relative.empty()) {
    return ec || CheckReparsePoint(PathToUtf8(root)) != ReparseState::kNo;
  }
  for (const auto &part : relative) {
    current /= part;
    const fs::file_status status = fs::symlink_status(current, ec);
    if (ec) {
      if (ec == std::errc::no_such_file_or_directory) {
        ec.clear();
        continue;
      }
      return true;
    }
    if (status.type() != fs::file_type::not_found &&
        CheckReparsePoint(PathToUtf8(current)) != ReparseState::kNo) {
      return true;
    }
  }
  return false;
}

VxCoreError ResolveAssetsRoot(Notebook *notebook, const std::string &file_path,
                              std::string &out_root) {
  const std::string &configured = notebook->GetConfig().assets_folder;
  const auto size = file_path.size();
  const bool protected_path = size >= 4 && file_path[size - 4] == '.' &&
      std::tolower(static_cast<unsigned char>(file_path[size - 3])) == 'v' &&
      std::tolower(static_cast<unsigned char>(file_path[size - 2])) == 'n' &&
      std::tolower(static_cast<unsigned char>(file_path[size - 1])) == 'e';
  if (configured.empty() || (!protected_path && !IsSafeRelativePath(configured, false))) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  const auto split = SplitPath(file_path);
  const fs::path notebook_root = PathFromUtf8(notebook->GetRootFolder()).lexically_normal();
  const fs::path parent = PathFromUtf8(notebook->GetAbsolutePath(split.first));
  const fs::path resolved = (parent / PathFromUtf8(configured)).lexically_normal();
  if (!IsPathWithin(notebook->GetRootFolder(), PathToUtf8(resolved), false) ||
      HasUnsafeComponent(notebook_root, resolved)) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  std::error_code ec;
  if (fs::exists(resolved, ec) && (!fs::is_directory(resolved, ec) || ec)) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  if (ec) {
    return VXCORE_ERR_IO;
  }
  out_root = PathToUtf8(resolved);
  return VXCORE_OK;
}

VxCoreError ValidateAttachments(const FileRecord &record) {
  const auto protection = record.CheckProtectionMetadata();
  if (protection != VXCORE_OK && protection != VXCORE_ERR_ENCRYPTION_LOCKED) return protection;
  for (const auto &attachment : record.attachments) {
    if (!IsSafeRelativePath(attachment, false)) {
      return VXCORE_ERR_INVALID_PARAM;
    }
  }
  return VXCORE_OK;
}

VxCoreError ValidateContainedPath(Notebook *notebook, const fs::path &path) {
  const fs::path root = PathFromUtf8(notebook->GetRootFolder()).lexically_normal();
  const std::string utf8_path = PathToUtf8(path.lexically_normal());
  if (!IsPathWithin(notebook->GetRootFolder(), utf8_path, false) ||
      HasUnsafeComponent(root, path.lexically_normal())) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  return VXCORE_OK;
}

VxCoreError HashPath(const fs::path &path, const std::string &label, uint64_t &hash,
                     const std::set<std::string> *excluded = nullptr) {
  if (excluded && excluded->count(CleanFsPath(path))) return VXCORE_OK;
  std::error_code ec;
  const fs::file_status status = fs::symlink_status(path, ec);
  if (ec) {
    return VXCORE_ERR_IO;
  }
  if (CheckReparsePoint(PathToUtf8(path)) != ReparseState::kNo) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  hash = FnvAppend(hash, label);
  if (fs::is_regular_file(status)) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
      return VXCORE_ERR_IO;
    }
    char buffer[64 * 1024];
    while (stream) {
      stream.read(buffer, sizeof(buffer));
      const std::streamsize count = stream.gcount();
      if (count > 0) {
        hash = FnvAppend(hash, buffer, static_cast<size_t>(count));
      }
    }
    return stream.eof() ? VXCORE_OK : VXCORE_ERR_IO;
  }
  if (!fs::is_directory(status)) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  std::vector<fs::path> entries;
  for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
    entries.push_back(it->path());
  }
  if (ec) {
    return VXCORE_ERR_IO;
  }
  std::sort(entries.begin(), entries.end(), [](const fs::path &left, const fs::path &right) {
    return PathToGenericUtf8(left.filename()) < PathToGenericUtf8(right.filename());
  });
  for (const auto &entry : entries) {
    const std::string child_label = label + "/" + PathToGenericUtf8(entry.filename());
    const VxCoreError error = HashPath(entry, child_label, hash, excluded);
    if (error != VXCORE_OK) {
      return error;
    }
  }
  return VXCORE_OK;
}

VxCoreError HashSource(const PreparedNodeTransferImpl &transfer, std::string &out_hash) {
  Notebook *source = transfer.source_override ? transfer.source_override
      : transfer.notebook_manager->GetNotebook(transfer.source_notebook_id);
  if (!source) {
    return VXCORE_ERR_NOT_FOUND;
  }
  auto *manager = dynamic_cast<BundledFolderManager *>(source->GetFolderManager());
  if (!manager) {
    return VXCORE_ERR_UNSUPPORTED;
  }
  uint64_t hash = 1469598103934665603ULL;
  const auto split = SplitPath(transfer.source_relative_path);
  FolderConfig *parent = nullptr;
  VxCoreError error = manager->TransferGetFolderConfig(split.first, &parent);
  if (error != VXCORE_OK || !parent) {
    return error == VXCORE_OK ? VXCORE_ERR_INVALID_STATE : error;
  }
  const size_t reachable_count =
      transfer.is_folder
          ? static_cast<size_t>(
                std::count(parent->folders.begin(), parent->folders.end(), split.second))
          : static_cast<size_t>(std::count_if(
                parent->files.begin(), parent->files.end(), [&](const FileRecord &file) {
                  return !transfer.files.empty() && file.name == split.second &&
                         file.id == transfer.files.front().source_record.id;
                }));
  if (reachable_count != 1) {
    return VXCORE_ERR_INVALID_STATE;
  }
  const fs::path parent_config = PathFromUtf8(manager->TransferGetConfigPath(split.first));
  error = ValidateContainedPath(source, parent_config);
  if (error != VXCORE_OK) {
    return error;
  }
  error = HashPath(parent_config, "parentConfig", hash);
  if (error != VXCORE_OK) {
    return error;
  }
  const fs::path content = PathFromUtf8(source->GetAbsolutePath(transfer.source_relative_path));
  error = ValidateContainedPath(source, content);
  if (error != VXCORE_OK) {
    return error;
  }
  error = HashPath(content, "content", hash);
  if (error != VXCORE_OK) {
    return error;
  }
  if (transfer.is_folder) {
    const fs::path metadata =
        PathFromUtf8(manager->TransferGetConfigPath(transfer.source_relative_path)).parent_path();
    error = ValidateContainedPath(source, metadata);
    if (error != VXCORE_OK) {
      return error;
    }
    error = HashPath(metadata, "metadata", hash);
  }
  if (error != VXCORE_OK) {
    return error;
  }
  std::set<std::string> seen;
  for (const auto &file : transfer.files) {
    const std::string asset_path = ConcatenatePaths(file.source_assets_root, file.source_record.id);
    if (!seen.insert(asset_path).second || !PathExists(asset_path)) {
      continue;
    }
    error = HashPath(PathFromUtf8(asset_path), "asset:" + file.relative_path, hash);
    if (error != VXCORE_OK) {
      return error;
    }
  }
  if (!transfer.is_folder && !transfer.files.empty() &&
      transfer.files.front().source_record.CheckProtectionMetadata() != VXCORE_OK) {
    const auto backup = PathFromUtf8(source->GetAbsolutePath(transfer.source_relative_path) + ".vswp");
    if (PathExists(PathToUtf8(backup))) {
      error = HashPath(backup, "backup", hash);
      if (error != VXCORE_OK) return error;
    }
  }
  MetadataStore *store = source->GetMetadataStore();
  if (!store && transfer.source_override && !transfer.destination_override) {
    out_hash = HashString(hash);
    return VXCORE_OK;  // Read-only bundle metadata is authoritative; no local DB exists.
  }
  if (!store) {
    return VXCORE_ERR_INVALID_STATE;
  }
  hash = FnvAppend(hash, "store");
  auto append_values = [&](const std::vector<std::string> &values) {
    hash = FnvAppend(hash, std::to_string(values.size()));
    for (const auto &value : values) {
      hash = FnvAppend(hash, value);
    }
  };
  for (const auto &folder : transfer.folders) {
    hash = FnvAppend(hash, "folder:" + folder.relative_path);
    const auto record = store->GetFolder(folder.source_config.id);
    hash = FnvAppend(hash, record ? "present" : "absent");
    if (record) {
      hash = FnvAppend(hash, record->id);
      hash = FnvAppend(hash, record->parent_id);
      hash = FnvAppend(hash, record->name);
      hash = FnvAppend(hash, std::to_string(record->created_utc));
      hash = FnvAppend(hash, std::to_string(record->modified_utc));
      hash = FnvAppend(hash, record->metadata);
    }
  }
  for (const auto &file : transfer.files) {
    hash = FnvAppend(hash, "file:" + file.relative_path);
    const auto record = store->GetFile(file.source_record.id);
    hash = FnvAppend(hash, record ? "present" : "absent");
    if (record) {
      hash = FnvAppend(hash, record->id);
      hash = FnvAppend(hash, record->folder_id);
      hash = FnvAppend(hash, record->name);
      hash = FnvAppend(hash, std::to_string(record->created_utc));
      hash = FnvAppend(hash, std::to_string(record->modified_utc));
      hash = FnvAppend(hash, record->metadata);
      append_values(store->GetFileTags(record->id));
      append_values(store->GetFileAttachments(record->id));
    }
  }
  out_hash = HashString(hash);
  return VXCORE_OK;
}

VxCoreError ReloadNotebookConfig(Notebook *notebook, const fs::path &config_path) {
  std::string bytes;
  if (ReadFile(config_path, bytes) != VXCORE_OK) {
    return VXCORE_ERR_IO;
  }
  try {
    NotebookConfig config = NotebookConfig::FromJson(nlohmann::json::parse(bytes));
    if (config.id.empty() || config.id != notebook->GetId()) {
      return VXCORE_ERR_INVALID_STATE;
    }
    const_cast<NotebookConfig &>(notebook->GetConfig()) = std::move(config);
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  }
}

uint64_t CountBytes(const fs::path &path) {
  std::error_code ec;
  if (fs::is_regular_file(path, ec)) {
    return static_cast<uint64_t>(fs::file_size(path, ec));
  }
  uint64_t total = 0;
  for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec),
       end;
       !ec && it != end; it.increment(ec)) {
    if (fs::is_regular_file(it->path(), ec)) {
      total += static_cast<uint64_t>(fs::file_size(it->path(), ec));
    }
  }
  return total;
}

VxCoreError CopyFileWithProgress(const fs::path &source, const fs::path &destination,
                                 const NodeTransferProgress &progress, uint64_t &completed,
                                 uint64_t total) {
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    return VXCORE_ERR_IO;
  }
  std::ifstream input(source, std::ios::binary);
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!input || !output) {
    return VXCORE_ERR_IO;
  }
  char buffer[64 * 1024];
  while (input) {
    input.read(buffer, sizeof(buffer));
    const std::streamsize count = input.gcount();
    if (count <= 0) {
      continue;
    }
    output.write(buffer, count);
    if (!output) {
      return VXCORE_ERR_IO;
    }
    completed += static_cast<uint64_t>(count);
    if (progress && !progress("copy", completed, total)) {
      return VXCORE_ERR_CANCELLED;
    }
  }
  return input.eof() ? VXCORE_OK : VXCORE_ERR_IO;
}

bool IsExcluded(const fs::path &path, const std::set<std::string> &excluded) {
  return excluded.count(CleanFsPath(path)) > 0;
}

VxCoreError CopyTree(const fs::path &source, const fs::path &destination,
                     const std::set<std::string> &excluded, const NodeTransferProgress &progress,
                     uint64_t &completed, uint64_t total) {
  if (CheckReparsePoint(PathToUtf8(source)) != ReparseState::kNo) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  std::error_code ec;
  const fs::file_status status = fs::symlink_status(source, ec);
  if (ec) {
    return VXCORE_ERR_IO;
  }
  if (fs::is_regular_file(status)) {
    return CopyFileWithProgress(source, destination, progress, completed, total);
  }
  if (!fs::is_directory(status)) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  fs::create_directories(destination, ec);
  if (ec) {
    return VXCORE_ERR_IO;
  }
  std::vector<fs::path> entries;
  for (fs::directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec)) {
    entries.push_back(it->path());
  }
  if (ec) {
    return VXCORE_ERR_IO;
  }
  std::sort(entries.begin(), entries.end(), [](const fs::path &left, const fs::path &right) {
    return PathToGenericUtf8(left.filename()) < PathToGenericUtf8(right.filename());
  });
  for (const auto &entry : entries) {
    if (IsExcluded(entry, excluded)) {
      continue;
    }
    const VxCoreError error =
        CopyTree(entry, destination / entry.filename(), excluded, progress, completed, total);
    if (error != VXCORE_OK) {
      return error;
    }
  }
  return VXCORE_OK;
}

std::string Extension(const std::string &name) {
  const size_t dot = name.find_last_of('.');
  return dot == std::string::npos || dot + 1 >= name.size() ? std::string() : name.substr(dot + 1);
}

std::string TransferName(const std::string &name, int suffix, bool is_folder) {
  if (suffix <= 1) {
    return name;
  }
  if (is_folder) {
    return name + " (" + std::to_string(suffix) + ")";
  }
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0) {
    return name + " (" + std::to_string(suffix) + ")";
  }
  return name.substr(0, dot) + " (" + std::to_string(suffix) + ")" + name.substr(dot);
}

VxCoreError SelectDestinationName(BundledFolderManager *manager, const std::string &parent_path,
                                  const std::string &desired, bool is_folder,
                                  std::string &out_name) {
  out_name.clear();
  FolderConfig *parent = nullptr;
  if (manager->TransferGetFolderConfig(parent_path, &parent) != VXCORE_OK || !parent) {
    return VXCORE_ERR_NOT_FOUND;
  }
  const fs::path content_parent = PathFromUtf8(manager->TransferGetContentPath(parent_path));
  std::vector<std::string> occupied;
  for (const auto &folder : parent->folders) {
    occupied.push_back(folder);
  }
  for (const auto &file : parent->files) {
    occupied.push_back(file.name);
  }
  std::error_code ec;
  for (fs::directory_iterator it(content_parent, ec), end; !ec && it != end; it.increment(ec)) {
    occupied.push_back(PathToUtf8(it->path().filename()));
  }
  if (ec) {
    return VXCORE_ERR_IO;
  }
  const fs::path metadata_parent =
      PathFromUtf8(manager->TransferGetConfigPath(ConcatenatePaths(parent_path, "candidate")))
          .parent_path()
          .parent_path();
  ec.clear();
  for (fs::directory_iterator it(metadata_parent, ec), end; !ec && it != end; it.increment(ec)) {
    occupied.push_back(PathToUtf8(it->path().filename()));
  }
  if (ec) {
    return VXCORE_ERR_IO;
  }
  // Collect existing names before creating our scratch directory, so it cannot
  // become its own occupied child (or unnecessarily double the probe path length).
  NameComparerProbe probe;
  VxCoreError error = CreateNameComparerProbe(content_parent, probe.path);
  if (error != VXCORE_OK) {
    return error;
  }
  for (const auto &occupied_name : occupied) {
    ec.clear();
    fs::create_directory(probe.path / PathFromUtf8(occupied_name), ec);
    if (ec && ec != std::errc::file_exists) {
      return VXCORE_ERR_IO;
    }
  }
  for (int suffix = 1; suffix <= 10000; ++suffix) {
    const std::string candidate = TransferName(desired, suffix, is_folder);
    bool candidate_exists = false;
    if (!PathExistsChecked(probe.path / PathFromUtf8(candidate), candidate_exists)) {
      return VXCORE_ERR_IO;
    }
    if (!candidate_exists) {
      out_name = candidate;
      return VXCORE_OK;
    }
  }
  return VXCORE_ERR_ALREADY_EXISTS;
}

std::string TagPath(const NotebookConfig &config, const std::string &tag_name) {
  std::vector<std::string> components;
  std::unordered_set<std::string> seen;
  std::string current = tag_name;
  while (!current.empty() && seen.insert(current).second) {
    components.push_back(current);
    auto it = std::find_if(config.tags.begin(), config.tags.end(),
                           [&](const TagNode &tag) { return tag.name == current; });
    if (it == config.tags.end()) {
      break;
    }
    current = it->parent;
  }
  std::reverse(components.begin(), components.end());
  std::string path;
  for (const auto &component : components) {
    path = path.empty() ? component : path + "/" + component;
  }
  return path;
}

VxCoreError CollectFolder(BundledFolderManager *manager, Notebook *source,
                          PreparedNodeTransferImpl &transfer,
                          const std::string &absolute_relative_path,
                          const std::string &inside_relative_path,
                          std::unordered_map<std::string, std::string> &ids) {
  FolderConfig *config = nullptr;
  VxCoreError error = manager->TransferGetFolderConfig(absolute_relative_path, &config);
  if (error != VXCORE_OK || !config) {
    return error;
  }
  IndexedFolder folder;
  folder.relative_path = inside_relative_path;
  folder.source_config = *config;
  folder.destination_config = *config;
  ids[config->id] = GenerateUUID();
  transfer.folders.push_back(folder);

  for (const auto &file_record : config->files) {
    const std::string source_file_path = ConcatenatePaths(absolute_relative_path, file_record.name);
    if (!IsRegularFile(source->GetAbsolutePath(source_file_path))) {
      return VXCORE_ERR_NODE_NOT_EXISTS;
    }
    error = ValidateAttachments(file_record);
    if (error != VXCORE_OK) {
      return error;
    }
    IndexedFile file;
    file.relative_path = ConcatenatePaths(inside_relative_path, file_record.name);
    file.source_record = file_record;
    file.destination_record = file_record;
    error = ResolveAssetsRoot(source, source_file_path, file.source_assets_root);
    if (error != VXCORE_OK) {
      return error;
    }
    ids[file_record.id] = GenerateUUID();
    transfer.files.push_back(std::move(file));
  }
  for (const auto &child : config->folders) {
    if (!IsSafeRelativePath(child, false) ||
        !IsDirectory(source->GetAbsolutePath(ConcatenatePaths(absolute_relative_path, child)))) {
      return VXCORE_ERR_NODE_NOT_EXISTS;
    }
    error =
        CollectFolder(manager, source, transfer, ConcatenatePaths(absolute_relative_path, child),
                      ConcatenatePaths(inside_relative_path, child), ids);
    if (error != VXCORE_OK) {
      return error;
    }
  }
  return VXCORE_OK;
}

void ApplyIdentities(PreparedNodeTransferImpl &transfer,
                     const std::unordered_map<std::string, std::string> &ids, int64_t timestamp) {
  for (auto &folder : transfer.folders) {
    folder.destination_config.id = ids.at(folder.source_config.id);
    if (!transfer.preserve_timestamps) {
      folder.destination_config.created_utc = timestamp;
      folder.destination_config.modified_utc = timestamp;
    }
    for (auto &record : folder.destination_config.files) {
      record.id = ids.at(record.id);
      if (!transfer.preserve_timestamps) {
        record.created_utc = timestamp;
        record.modified_utc = timestamp;
      }
    }
  }
  for (auto &file : transfer.files) {
    file.destination_record.id = ids.at(file.source_record.id);
    if (!transfer.preserve_timestamps) {
      file.destination_record.created_utc = timestamp;
      file.destination_record.modified_utc = timestamp;
    }
  }
  transfer.destination_node_id = transfer.is_folder ? transfer.folders.front().destination_config.id
                                                    : transfer.files.front().destination_record.id;
}

VxCoreError WriteStagedMetadata(const PreparedNodeTransferImpl &transfer) {
  const fs::path metadata_root = PathFromUtf8(transfer.staging_dir) / kMetadataName;
  for (const auto &folder : transfer.folders) {
    fs::path path = metadata_root;
    if (!folder.relative_path.empty() && folder.relative_path != ".") {
      path /= PathFromUtf8(folder.relative_path);
    }
    if (!WriteDurable(path / "vx.json", folder.destination_config.ToJson().dump(2))) {
      return VXCORE_ERR_IO;
    }
  }
  return VXCORE_OK;
}

nlohmann::json ToManifest(const PreparedNodeTransferImpl &transfer) {
  nlohmann::json manifest;
  manifest["sourceNotebookId"] = transfer.source_notebook_id;
  manifest["sourceRelativePath"] = transfer.source_relative_path;
  manifest["destinationNotebookId"] = transfer.destination_notebook_id;
  manifest["destinationFolderPath"] = transfer.destination_folder_path;
  manifest["operation"] = transfer.operation;
  manifest["isFolder"] = transfer.is_folder;
  manifest["sourceName"] = transfer.source_name;
  manifest["destinationNodeId"] = transfer.destination_node_id;
  manifest["sourceFingerprint"] = transfer.source_fingerprint;
  manifest["tagPaths"] = transfer.tag_paths;
  manifest["files"] = nlohmann::json::array();
  for (const auto &file : transfer.files) {
    manifest["files"].push_back({{"relativePath", file.relative_path},
                                 {"source", file.source_record.ToJson()},
                                 {"destination", file.destination_record.ToJson()},
                                 {"sourceAssetsRoot", file.source_assets_root},
                                 {"stagedAssetsRoot", file.staged_assets_root}});
  }
  manifest["folders"] = nlohmann::json::array();
  for (const auto &folder : transfer.folders) {
    manifest["folders"].push_back(
        {{"relativePath", folder.relative_path}, {"source", folder.source_config.ToJson()}});
  }
  return manifest;
}

StoreFolderRecord StoreFolder(const FolderConfig &config, const std::string &parent_id) {
  return {config.id,          parent_id,           config.name,
          config.created_utc, config.modified_utc, config.metadata.dump()};
}

StoreFileRecord StoreFile(const FileRecord &file, const std::string &folder_id) {
  return {file.id,          folder_id,         file.name,
          file.created_utc, file.modified_utc, file.metadata.dump(),
          file.tags,        file.attachments};
}

VxCoreError InsertDestinationStore(Notebook *notebook, const PreparedNodeTransferImpl &transfer,
                                   const std::string &destination_name,
                                   const std::vector<TagNode> &created_tags,
                                   const std::string &destination_parent_id) {
  MetadataStore *store = notebook->GetMetadataStore();
  if (!store) {
    return VXCORE_ERR_INVALID_STATE;
  }
  if (!store->BeginTransaction()) {
    return VXCORE_ERR_DATABASE;
  }
  for (const auto &tag : created_tags) {
    if (!store->CreateOrUpdateTag({tag.name, tag.parent, tag.metadata.dump()})) {
      store->RollbackTransaction();
      return VXCORE_ERR_DATABASE;
    }
  }
  if (!transfer.is_folder) {
    FileRecord file = transfer.files.front().destination_record;
    file.name = destination_name;
    VxCoreError error = store->InsertFile(StoreFile(file, destination_parent_id));
    if (error != VXCORE_OK ||
        (!file.attachments.empty() && !store->SetFileAttachments(file.id, file.attachments))) {
      store->RollbackTransaction();
      return error == VXCORE_OK ? VXCORE_ERR_DATABASE : error;
    }
  } else {
    std::unordered_map<std::string, std::string> folder_ids;
    for (const auto &folder : transfer.folders) {
      folder_ids[folder.relative_path] = folder.destination_config.id;
    }
    for (const auto &folder : transfer.folders) {
      FolderConfig config = folder.destination_config;
      if (folder.relative_path.empty() || folder.relative_path == ".") {
        config.name = destination_name;
      }
      std::string parent_id = destination_parent_id;
      if (!folder.relative_path.empty() && folder.relative_path != ".") {
        parent_id = folder_ids[SplitPath(folder.relative_path).first];
      }
      const VxCoreError error = store->InsertFolder(StoreFolder(config, parent_id));
      if (error != VXCORE_OK) {
        store->RollbackTransaction();
        return error;
      }
      for (const auto &file : config.files) {
        const VxCoreError file_error = store->InsertFile(StoreFile(file, config.id));
        if (file_error != VXCORE_OK ||
            (!file.attachments.empty() && !store->SetFileAttachments(file.id, file.attachments))) {
          store->RollbackTransaction();
          return file_error == VXCORE_OK ? VXCORE_ERR_DATABASE : file_error;
        }
      }
    }
  }
  if (store->CommitTransaction()) {
    return VXCORE_OK;
  }
  return store->RollbackTransaction() ? VXCORE_ERR_DATABASE : VXCORE_ERR_INVALID_STATE;
}

VxCoreError DeleteStoreIds(Notebook *notebook, const PreparedNodeTransferImpl &transfer,
                           const std::vector<TagNode> &created_tags) {
  MetadataStore *store = notebook->GetMetadataStore();
  if (!store || !store->BeginTransaction()) {
    return VXCORE_ERR_DATABASE;
  }
  bool ok = true;
  for (const auto &file : transfer.files) {
    if (store->GetFile(file.destination_record.id) &&
        !store->DeleteFile(file.destination_record.id)) {
      ok = false;
      break;
    }
  }
  if (ok) {
    for (auto it = transfer.folders.rbegin(); it != transfer.folders.rend(); ++it) {
      if (store->GetFolder(it->destination_config.id) &&
          !store->DeleteFolder(it->destination_config.id)) {
        ok = false;
        break;
      }
    }
  }
  if (ok) {
    for (auto it = created_tags.rbegin(); it != created_tags.rend(); ++it) {
      if (store->GetTag(it->name) && !store->DeleteTag(it->name)) {
        ok = false;
        break;
      }
    }
  }
  if (!ok) {
    return store->RollbackTransaction() ? VXCORE_ERR_DATABASE : VXCORE_ERR_INVALID_STATE;
  }
  if (store->CommitTransaction()) {
    return VXCORE_OK;
  }
  return store->RollbackTransaction() ? VXCORE_ERR_DATABASE : VXCORE_ERR_INVALID_STATE;
}

VxCoreError DeleteJournalStoreIds(Notebook *notebook, const nlohmann::json &journal,
                                  bool delete_tags) {
  MetadataStore *store = notebook->GetMetadataStore();
  if (!store || !journal.contains("ids") || !journal["ids"].is_array() ||
      !store->BeginTransaction()) {
    return VXCORE_ERR_DATABASE;
  }
  bool ok = true;
  for (const auto &id_json : journal["ids"]) {
    if (!id_json.is_string()) {
      ok = false;
      break;
    }
    const std::string id = id_json.get<std::string>();
    if (store->GetFile(id) && !store->DeleteFile(id)) {
      ok = false;
      break;
    }
  }
  if (ok) {
    for (auto it = journal["ids"].rbegin(); it != journal["ids"].rend(); ++it) {
      const std::string id = it->get<std::string>();
      if (store->GetFolder(id) && !store->DeleteFolder(id)) {
        ok = false;
        break;
      }
    }
  }
  if (ok && delete_tags && journal.contains("createdTags")) {
    if (!journal["createdTags"].is_array()) {
      ok = false;
    } else {
      for (auto it = journal["createdTags"].rbegin(); ok && it != journal["createdTags"].rend();
           ++it) {
        if (!it->is_string()) {
          ok = false;
          break;
        }
        const std::string name = it->get<std::string>();
        if (store->GetTag(name) && !store->DeleteTag(name)) {
          ok = false;
        }
      }
    }
  }
  if (!ok) {
    return store->RollbackTransaction() ? VXCORE_ERR_DATABASE : VXCORE_ERR_INVALID_STATE;
  }
  if (store->CommitTransaction()) {
    return VXCORE_OK;
  }
  return store->RollbackTransaction() ? VXCORE_ERR_DATABASE : VXCORE_ERR_INVALID_STATE;
}

VxCoreError FingerprintDestination(Notebook *notebook, BundledFolderManager *manager,
                                   const std::string &path, bool is_folder,
                                   const std::vector<IndexedFile> &files, std::string &out_hash) {
  uint64_t hash = 1469598103934665603ULL;
  // CopyTree stages owned assets separately. Do not hash those bytes once as
  // published content and again as assets when the target is a folder.
  std::set<std::string> excluded;
  if (is_folder) {
    for (const auto &file : files) {
      const auto destination_file = ConcatenatePaths(path, file.relative_path);
      const auto parent = PathFromUtf8(notebook->GetAbsolutePath(SplitPath(destination_file).first));
      excluded.insert(CleanFsPath(parent / PathFromUtf8(notebook->GetConfig().assets_folder)));
    }
  }
  VxCoreError error = HashPath(PathFromUtf8(notebook->GetAbsolutePath(path)), "content", hash,
                               is_folder ? &excluded : nullptr);
  if (error != VXCORE_OK) {
    return error;
  }
  if (is_folder) {
    error = HashPath(PathFromUtf8(manager->TransferGetConfigPath(path)).parent_path(), "metadata",
                     hash);
  } else {
    const FileRecord *record = nullptr;
    error = manager->GetFileInfo(path, &record);
    if (error == VXCORE_OK && record) {
      hash = FnvAppend(hash, "metadata");
      hash = FnvAppend(hash, record->ToJson().dump());
    } else if (error == VXCORE_OK) {
      error = VXCORE_ERR_INVALID_STATE;
    }
  }
  if (error != VXCORE_OK) {
    return error;
  }
  for (const auto &file : files) {
    std::string destination_file_path =
        is_folder ? ConcatenatePaths(path, file.relative_path) : path;
    std::string assets_root;
    error = ResolveAssetsRoot(notebook, destination_file_path, assets_root);
    if (error != VXCORE_OK) {
      return error;
    }
    const std::string assets = ConcatenatePaths(assets_root, file.destination_record.id);
    if (PathExists(assets)) {
      error = HashPath(PathFromUtf8(assets), "asset:" + file.relative_path, hash);
      if (error != VXCORE_OK) {
        return error;
      }
    }
  }
  if (!is_folder && !files.empty() &&
      files.front().destination_record.CheckProtectionMetadata() != VXCORE_OK) {
    const auto backup = PathFromUtf8(notebook->GetAbsolutePath(path) + ".vswp");
    if (PathExists(PathToUtf8(backup))) {
      error = HashPath(backup, "backup", hash);
      if (error != VXCORE_OK) return error;
    }
  }
  out_hash = HashString(hash);
  return VXCORE_OK;
}

VxCoreError FingerprintStagedDestination(const PreparedNodeTransferImpl &transfer,
                                         const std::string &destination_name,
                                         std::string &out_hash) {
  uint64_t hash = 1469598103934665603ULL;
  const fs::path staging = PathFromUtf8(transfer.staging_dir);
  VxCoreError error = HashPath(staging / kContentName, "content", hash);
  if (error != VXCORE_OK) {
    return error;
  }
  if (transfer.is_folder) {
    error = HashPath(staging / kMetadataName, "metadata", hash);
  } else {
    FileRecord record = transfer.files.front().destination_record;
    record.name = destination_name;
    hash = FnvAppend(hash, "metadata");
    hash = FnvAppend(hash, record.ToJson().dump());
  }
  if (error != VXCORE_OK) {
    return error;
  }
  for (const auto &file : transfer.files) {
    if (file.staged_assets_root.empty()) {
      continue;
    }
    error = HashPath(PathFromUtf8(file.staged_assets_root), "asset:" + file.relative_path, hash);
    if (error != VXCORE_OK) {
      return error;
    }
  }
  if (!transfer.is_folder && !transfer.files.empty() && transfer.files.front().has_backup) {
    error = HashPath(staging / kAssetsName / std::to_string(transfer.files.size()), "backup", hash);
    if (error != VXCORE_OK) return error;
  }
  out_hash = HashString(hash);
  return VXCORE_OK;
}

void AppendEvent(nlohmann::json &events, const char *name, nlohmann::json data) {
  events.push_back({{"name", name}, {"data", std::move(data)}});
}

struct FreeCString {
  void operator()(char *value) const { std::free(value); }
};

using PreparedCString = std::unique_ptr<char, FreeCString>;

PreparedCString PrepareResultString(nlohmann::json result, const std::string &event_batch_id) {
  result["eventBatchId"] = event_batch_id;
  const std::string serialized = result.dump();
  PreparedCString prepared(static_cast<char *>(std::malloc(serialized.size() + 1)));
  if (prepared) {
    std::memcpy(prepared.get(), serialized.c_str(), serialized.size() + 1);
  }
  return prepared;
}

void SelectPreparedResult(char **out_result_json, PreparedCString &prepared) noexcept {
  std::free(*out_result_json);
  *out_result_json = prepared.release();
}

bool CanonicallyEqual(const fs::path &left, const fs::path &right) {
  const std::string left_value = PathToUtf8(left);
  const std::string right_value = PathToUtf8(right);
  return !left.empty() && !right.empty() && IsPathWithin(left_value, right_value, false) &&
         IsPathWithin(right_value, left_value, false);
}

bool ValidateAbsoluteJournalPath(const nlohmann::json &owner, const char *key, const fs::path &root,
                                 const fs::path *expected, bool allow_empty = false) {
  if (!owner.contains(key) || !owner[key].is_string()) {
    return false;
  }
  const std::string value = owner[key].get<std::string>();
  if (value.empty()) {
    return allow_empty;
  }
  const fs::path path = PathFromUtf8(value);
  return path.is_absolute() && IsPathWithin(PathToUtf8(root), value, false) &&
         (!expected || CanonicallyEqual(path, *expected));
}

bool ValidateJournalAssetPath(Notebook *notebook, const nlohmann::json &asset,
                              const std::string &expected_file_path, const char *path_key) {
  const std::string id = asset.value("id", std::string());
  if (id.empty() || asset.value("filePath", std::string()) != expected_file_path) {
    return false;
  }
  if (asset.value("backup", false)) {
    const auto expected = PathFromUtf8(notebook->GetAbsolutePath(expected_file_path) + ".vswp");
    const auto root = PathFromUtf8(notebook->GetRootFolder());
    return ValidateAbsoluteJournalPath(asset, path_key, root, &expected);
  }
  std::string assets_root;
  if (ResolveAssetsRoot(notebook, expected_file_path, assets_root) != VXCORE_OK) {
    return false;
  }
  const fs::path expected = PathFromUtf8(ConcatenatePaths(assets_root, id));
  const fs::path notebook_root = PathFromUtf8(notebook->GetRootFolder());
  return ValidateAbsoluteJournalPath(asset, path_key, notebook_root, &expected);
}

bool IsDecimalIndex(const std::string &value) {
  return !value.empty() && std::all_of(value.begin(), value.end(),
                                       [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

VxCoreError ValidateRecoveryJournal(Notebook *notebook, BundledFolderManager *manager,
                                    const fs::path &transfer_root, const fs::path &entry,
                                    const nlohmann::json &journal) {
  if (!IsPathWithin(PathToUtf8(transfer_root), PathToUtf8(entry), false) ||
      CheckReparsePoint(PathToUtf8(entry)) != ReparseState::kNo) {
    return VXCORE_ERR_INVALID_STATE;
  }
  const fs::path notebook_root = PathFromUtf8(notebook->GetRootFolder());
  if (CheckReparsePoint(PathToUtf8(notebook_root)) != ReparseState::kNo ||
      HasUnsafeComponent(notebook_root, entry)) {
    return VXCORE_ERR_INVALID_STATE;
  }
  const std::string kind = journal.value("kind", std::string());
  if (kind == kKindDestination) {
    const std::string destination_path = journal.value("destinationPath", std::string());
    const std::string parent_path = journal.value("destinationParentPath", std::string());
    if (!IsSafeRelativePath(destination_path, false) || !IsSafeRelativePath(parent_path, true) ||
        CleanPath(SplitPath(destination_path).first) != CleanPath(parent_path)) {
      return VXCORE_ERR_INVALID_STATE;
    }
    const std::string phase = journal.value("phase", std::string());
    if (phase != kPhaseInit && phase != kPhasePublished && phase != kPhaseDb &&
        phase != kPhaseCommitted) {
      return VXCORE_ERR_INVALID_STATE;
    }
    const fs::path expected_content = PathFromUtf8(notebook->GetAbsolutePath(destination_path));
    if (!ValidateAbsoluteJournalPath(journal, "contentTarget", notebook_root, &expected_content)) {
      return VXCORE_ERR_INVALID_STATE;
    }
    const bool is_folder = journal.value("isFolder", false);
    const fs::path expected_metadata =
        PathFromUtf8(manager->TransferGetConfigPath(destination_path)).parent_path();
    if (!ValidateAbsoluteJournalPath(journal, "metadataTarget", notebook_root,
                                     is_folder ? &expected_metadata : nullptr, !is_folder) ||
        (!is_folder && journal["metadataTarget"].get<std::string>() != "")) {
      return VXCORE_ERR_INVALID_STATE;
    }
    if (HasUnsafeComponent(notebook_root, expected_content) ||
        (is_folder && HasUnsafeComponent(notebook_root, expected_metadata))) {
      return VXCORE_ERR_INVALID_STATE;
    }
    if (!journal.contains("assets") || !journal["assets"].is_array()) {
      return VXCORE_ERR_INVALID_STATE;
    }
    std::set<std::string> slots;
    for (const auto &asset : journal["assets"]) {
      const std::string relative_path = asset.value("relativePath", std::string());
      const std::string file_path =
          is_folder ? ConcatenatePaths(destination_path, relative_path) : destination_path;
      const std::string slot = asset.value("slot", std::string());
      const fs::path expected_staged = entry / kAssetsName / slot;
      if (!asset.is_object() || !IsSafeRelativePath(relative_path, false) ||
          !IsDecimalIndex(slot) || !slots.insert(slot).second ||
          !ValidateAbsoluteJournalPath(asset, "staged", entry, &expected_staged) ||
          !ValidateJournalAssetPath(notebook, asset, file_path, "target") ||
          HasUnsafeComponent(entry, expected_staged) ||
          HasUnsafeComponent(notebook_root, PathFromUtf8(asset["target"].get<std::string>()))) {
        return VXCORE_ERR_INVALID_STATE;
      }
    }
    return VXCORE_OK;
  }
  if (kind != kKindSourceRemoval) {
    return VXCORE_ERR_INVALID_STATE;
  }
  const std::string source_path = journal.value("sourceRelativePath", std::string());
  const std::string parent_path = journal.value("sourceParentPath", std::string());
  if (!IsSafeRelativePath(source_path, false) || !IsSafeRelativePath(parent_path, true) ||
      CleanPath(SplitPath(source_path).first) != CleanPath(parent_path)) {
    return VXCORE_ERR_INVALID_STATE;
  }
  const std::string phase = journal.value("phase", std::string());
  if (phase != kPhaseInit && phase != kPhaseQuarantined && phase != kPhaseSourceCommitted) {
    return VXCORE_ERR_INVALID_STATE;
  }
  const fs::path expected_content = PathFromUtf8(notebook->GetAbsolutePath(source_path));
  const fs::path expected_content_quarantine = entry / "quarantine" / "content";
  if (!ValidateAbsoluteJournalPath(journal, "contentSource", notebook_root, &expected_content) ||
      !ValidateAbsoluteJournalPath(journal, "contentQuarantine", entry,
                                   &expected_content_quarantine)) {
    return VXCORE_ERR_INVALID_STATE;
  }
  const bool is_folder = journal.value("isFolder", false);
  const fs::path expected_metadata =
      PathFromUtf8(manager->TransferGetConfigPath(source_path)).parent_path();
  const fs::path expected_metadata_quarantine = entry / "quarantine" / "metadata";
  if (!ValidateAbsoluteJournalPath(journal, "metadataSource", notebook_root,
                                   is_folder ? &expected_metadata : nullptr, !is_folder) ||
      !ValidateAbsoluteJournalPath(journal, "metadataQuarantine", entry,
                                   is_folder ? &expected_metadata_quarantine : nullptr,
                                   !is_folder) ||
      (!is_folder && (journal["metadataSource"].get<std::string>() != "" ||
                      journal["metadataQuarantine"].get<std::string>() != ""))) {
    return VXCORE_ERR_INVALID_STATE;
  }
  if (HasUnsafeComponent(notebook_root, expected_content) ||
      HasUnsafeComponent(entry, expected_content_quarantine) ||
      (is_folder && (HasUnsafeComponent(notebook_root, expected_metadata) ||
                     HasUnsafeComponent(entry, expected_metadata_quarantine)))) {
    return VXCORE_ERR_INVALID_STATE;
  }
  if (!journal.contains("assets") || !journal["assets"].is_array()) {
    return VXCORE_ERR_INVALID_STATE;
  }
  std::set<std::string> slots;
  for (const auto &asset : journal["assets"]) {
    const std::string relative_path = asset.value("relativePath", std::string());
    const std::string file_path =
        is_folder ? ConcatenatePaths(source_path, relative_path) : source_path;
    const std::string slot = asset.value("slot", std::string());
    const fs::path expected_quarantine = entry / "quarantine" / kAssetsName / slot;
    if (!asset.is_object() || !IsSafeRelativePath(relative_path, false) || !IsDecimalIndex(slot) ||
        !slots.insert(slot).second ||
        !ValidateJournalAssetPath(notebook, asset, file_path, "source") ||
        !ValidateAbsoluteJournalPath(asset, "quarantine", entry, &expected_quarantine) ||
        HasUnsafeComponent(notebook_root, PathFromUtf8(asset["source"].get<std::string>())) ||
        HasUnsafeComponent(entry, expected_quarantine)) {
      return VXCORE_ERR_INVALID_STATE;
    }
  }
  return VXCORE_OK;
}

enum class SourceRemovalState { Removed, RestoredSourceRetained, RecoveryRequired };

struct SourceRemovalOutcome {
  SourceRemovalState state = SourceRemovalState::RecoveryRequired;
  VxCoreError error = VXCORE_ERR_INVALID_STATE;
};

SourceRemovalOutcome RemoveSource(PreparedNodeTransferImpl &transfer, std::string &out_error) {
  auto recovery_required = [&](VxCoreError error, const char *message) {
    out_error = message;
    return SourceRemovalOutcome{SourceRemovalState::RecoveryRequired, error};
  };
  Notebook *source = transfer.notebook_manager->GetNotebook(transfer.source_notebook_id);
  if (!source) {
    return recovery_required(VXCORE_ERR_NOT_FOUND, "Source notebook is no longer open");
  }
  if (source->CheckWritable() != VXCORE_OK) {
    return recovery_required(source->CheckWritable(), "Source notebook cannot be mutated");
  }
  auto *manager = dynamic_cast<BundledFolderManager *>(source->GetFolderManager());
  if (!manager) {
    return recovery_required(VXCORE_ERR_UNSUPPORTED, "Source notebook is unsupported");
  }
  std::string current_fingerprint;
  VxCoreError error = HashSource(transfer, current_fingerprint);
  if (error != VXCORE_OK || current_fingerprint != transfer.source_fingerprint) {
    return recovery_required(error == VXCORE_OK ? VXCORE_ERR_INVALID_STATE : error,
                             "Source changed after the transfer snapshot");
  }

  const auto split = SplitPath(transfer.source_relative_path);
  FolderConfig *parent = nullptr;
  error = manager->TransferGetFolderConfig(split.first, &parent);
  if (error != VXCORE_OK || !parent) {
    return recovery_required(error == VXCORE_OK ? VXCORE_ERR_INVALID_STATE : error,
                             "Source parent is unavailable");
  }
  const FolderConfig original_parent = *parent;
  const fs::path source_parent_config_path =
      PathFromUtf8(manager->TransferGetConfigPath(split.first));
  std::string source_parent_bytes;
  if (ReadFile(source_parent_config_path, source_parent_bytes) != VXCORE_OK) {
    return recovery_required(VXCORE_ERR_IO, "Source parent config could not be read");
  }
  const fs::path journal_dir = PathFromUtf8(source->GetMetadataFolder()) / kTransferDirName /
                               PathFromUtf8(transfer.source_recovery_id);
  const fs::path quarantine = journal_dir / "quarantine";
  const fs::path content_source =
      PathFromUtf8(source->GetAbsolutePath(transfer.source_relative_path));
  const fs::path content_quarantine = quarantine / "content";
  const fs::path metadata_source =
      PathFromUtf8(manager->TransferGetConfigPath(transfer.source_relative_path)).parent_path();
  const fs::path metadata_quarantine = quarantine / "metadata";
  nlohmann::json journal = {{"kind", kKindSourceRemoval},
                            {"phase", kPhaseInit},
                            {"sourceRelativePath", transfer.source_relative_path},
                            {"sourceParentPath", split.first},
                            {"sourceParentConfig", original_parent.ToJson()},
                            {"sourceParentConfigBytes", source_parent_bytes},
                            {"isFolder", transfer.is_folder},
                            {"contentSource", PathToUtf8(content_source)},
                            {"contentQuarantine", PathToUtf8(content_quarantine)},
                            {"metadataSource", ""},
                            {"metadataQuarantine", ""},
                            {"assets", nlohmann::json::array()}};
  if (transfer.is_folder) {
    journal["metadataSource"] = PathToUtf8(metadata_source);
    journal["metadataQuarantine"] = PathToUtf8(metadata_quarantine);
  }
  journal["ids"] = nlohmann::json::array();
  for (const auto &folder : transfer.folders) {
    journal["ids"].push_back(folder.source_config.id);
  }
  for (const auto &file : transfer.files) {
    journal["ids"].push_back(file.source_record.id);
  }
  for (size_t i = 0; i < transfer.files.size(); ++i) {
    const auto &file = transfer.files[i];
    const fs::path source_asset =
        PathFromUtf8(ConcatenatePaths(file.source_assets_root, file.source_record.id));
    if (PathExists(PathToUtf8(source_asset)) &&
        !IsPathWithin(PathToUtf8(content_source), PathToUtf8(source_asset), false)) {
      journal["assets"].push_back(
          {{"source", PathToUtf8(source_asset)},
           {"relativePath", file.relative_path},
           {"filePath", transfer.is_folder
                            ? ConcatenatePaths(transfer.source_relative_path, file.relative_path)
                            : transfer.source_relative_path},
           {"id", file.source_record.id},
           {"slot", std::to_string(i)},
           {"quarantine", PathToUtf8(quarantine / "assets" / std::to_string(i))}});
    }
    if (!transfer.is_folder &&
        file.source_record.CheckProtectionMetadata() != VXCORE_OK) {
      const auto backup = PathFromUtf8(PathToUtf8(content_source) + ".vswp");
      if (PathExists(PathToUtf8(backup))) {
        const auto slot = std::to_string(transfer.files.size() + i);
        journal["assets"].push_back({{"source", PathToUtf8(backup)},
            {"relativePath", file.relative_path}, {"filePath", transfer.source_relative_path},
            {"id", file.source_record.id}, {"slot", slot}, {"backup", true},
            {"quarantine", PathToUtf8(quarantine / "assets" / slot)}});
      }
    }
  }
  const fs::path journal_path = journal_dir / kJournalName;
  auto save_phase = [&](const char *phase) {
    journal["phase"] = phase;
    return WriteDurable(journal_path, journal.dump(2));
  };
  auto prove_restored = [&](VxCoreError failure) {
    manager->TransferInvalidateCache(transfer.source_relative_path);
    manager->TransferInvalidateCache(split.first);
    std::string restored_fingerprint;
    const VxCoreError verify_error = HashSource(transfer, restored_fingerprint);
    if (verify_error != VXCORE_OK || restored_fingerprint != transfer.source_fingerprint) {
      return recovery_required(verify_error == VXCORE_OK ? VXCORE_ERR_INVALID_STATE : verify_error,
                               "Source restoration is uncertain; recovery is required");
    }
    if (!CleanupJournalDir(journal_dir)) {
      VXCORE_LOG_WARN("NodeTransfer: verified source retained; journal cleanup deferred: %s",
                      PathToUtf8(journal_dir).c_str());
    }
    out_error = "Destination was copied and the original source was retained";
    return SourceRemovalOutcome{SourceRemovalState::RestoredSourceRetained, failure};
  };
  auto restore_paths = [&](bool inject_content_failure = false) {
    bool restored = true;
    for (const auto &asset : journal["assets"]) {
      restored = RestoreRenamedPath(PathFromUtf8(asset["quarantine"].get<std::string>()),
                                    PathFromUtf8(asset["source"].get<std::string>())) &&
                 restored;
    }
    if (transfer.is_folder) {
      restored = RestoreRenamedPath(metadata_quarantine, metadata_source) && restored;
    }
    if (inject_content_failure) {
      restored = false;
    } else {
      restored = RestoreRenamedPath(content_quarantine, content_source) && restored;
    }
    return restored;
  };
  if (!save_phase(kPhaseInit) || !RenamePath(content_source, content_quarantine)) {
    return prove_restored(VXCORE_ERR_IO);
  }
  if (transfer.is_folder && !RenamePath(metadata_source, metadata_quarantine)) {
    restore_paths();
    return prove_restored(VXCORE_ERR_IO);
  }
  for (const auto &asset : journal["assets"]) {
    if (!RenamePath(PathFromUtf8(asset["source"].get<std::string>()),
                    PathFromUtf8(asset["quarantine"].get<std::string>()))) {
      restore_paths();
      return prove_restored(VXCORE_ERR_IO);
    }
  }
  if (!save_phase(kPhaseQuarantined)) {
    restore_paths();
    return prove_restored(VXCORE_ERR_IO);
  }
  if (transfer.test_throw_after_source_quarantine) {
    throw std::runtime_error("Injected exception after source quarantine");
  }
  if (transfer.test_fail_source_removal || transfer.test_fail_source_rollback_content) {
    restore_paths(transfer.test_fail_source_rollback_content);
    return prove_restored(VXCORE_ERR_IO);
  }

  FolderConfig updated = original_parent;
  if (transfer.is_folder) {
    auto it = std::find(updated.folders.begin(), updated.folders.end(), split.second);
    if (it == updated.folders.end()) {
      restore_paths();
      return prove_restored(VXCORE_ERR_INVALID_STATE);
    }
    updated.folders.erase(it);
  } else {
    auto it = std::find_if(updated.files.begin(), updated.files.end(), [&](const FileRecord &file) {
      return file.id == transfer.files[0].source_record.id;
    });
    if (it == updated.files.end()) {
      restore_paths();
      return prove_restored(VXCORE_ERR_INVALID_STATE);
    }
    updated.files.erase(it);
  }
  updated.modified_utc = GetCurrentTimestampMillis();
  if (!ReplaceDurable(source_parent_config_path, updated.ToJson().dump(2))) {
    restore_paths();
    return prove_restored(VXCORE_ERR_IO);
  }
  if (!save_phase(kPhaseSourceCommitted)) {
    ReplaceDurable(source_parent_config_path, source_parent_bytes);
    restore_paths();
    return prove_restored(VXCORE_ERR_IO);
  }
  auto rollback_source_commit = [&](VxCoreError failure) {
    const bool phase_saved = save_phase(kPhaseQuarantined);
    if (!transfer.test_fail_source_rollback_parent) {
      ReplaceDurable(source_parent_config_path, source_parent_bytes);
    }
    restore_paths();
    if (!phase_saved) {
      return recovery_required(VXCORE_ERR_INVALID_STATE,
                               "Source recovery journal could not be advanced");
    }
    return prove_restored(failure);
  };
  MetadataStore *store = source->GetMetadataStore();
  if (transfer.test_fail_source_rollback_parent || !store || !store->BeginTransaction()) {
    return rollback_source_commit(VXCORE_ERR_DATABASE);
  }
  const bool store_deleted = transfer.is_folder
                                 ? (!store->GetFolder(transfer.folders.front().source_config.id) ||
                                    store->DeleteFolder(transfer.folders.front().source_config.id))
                                 : (!store->GetFile(transfer.files.front().source_record.id) ||
                                    store->DeleteFile(transfer.files.front().source_record.id));
  if (!store_deleted || !store->CommitTransaction()) {
    const bool store_rolled_back = store->RollbackTransaction();
    if (!store_rolled_back) {
      return recovery_required(VXCORE_ERR_INVALID_STATE,
                               "Source store rollback failed; recovery is required");
    }
    return rollback_source_commit(VXCORE_ERR_DATABASE);
  }
  try {
    manager->TransferInvalidateCache(transfer.source_relative_path);
    manager->TransferInvalidateCache(split.first);
    if (!CleanupJournalDir(journal_dir)) {
      VXCORE_LOG_WARN("NodeTransfer: source removal committed; quarantine cleanup deferred: %s",
                      PathToUtf8(journal_dir).c_str());
    }
  } catch (...) {
    // Source deletion is already durable. Cache cleanup cannot change that fact.
  }
  return {SourceRemovalState::Removed, VXCORE_OK};
}

}  // namespace

PreparedNodeTransfer::~PreparedNodeTransfer() = default;

NodeTransferCommitResult::~NodeTransferCommitResult() { Reset(); }

void NodeTransferCommitResult::Reset() noexcept {
  std::free(success_json);
  std::free(retained_json);
  std::free(recovery_json);
  success_json = nullptr;
  retained_json = nullptr;
  recovery_json = nullptr;
  outcome = NodeTransferCommitOutcome::NotCommitted;
}

bool NodeTransferCommitResult::HasCommittedDestination() const noexcept {
  return outcome != NodeTransferCommitOutcome::NotCommitted;
}

char *NodeTransferCommitResult::ReleaseSelected() noexcept {
  char **selected = nullptr;
  if (outcome == NodeTransferCommitOutcome::Success) {
    selected = &success_json;
  } else if (outcome == NodeTransferCommitOutcome::SourceRetained) {
    selected = &retained_json;
  } else if (outcome == NodeTransferCommitOutcome::RecoveryRequired ||
             outcome == NodeTransferCommitOutcome::DestinationCommitted) {
    selected = &recovery_json;
  }
  if (!selected) {
    return nullptr;
  }
  char *result = *selected;
  *selected = nullptr;
  return result;
}

char *NodeTransferCommitResult::ReleaseRecovery() noexcept {
  outcome = NodeTransferCommitOutcome::RecoveryRequired;
  char *result = recovery_json;
  recovery_json = nullptr;
  return result;
}

VxCoreError NodeTransfer::Prepare(
    NotebookManager *notebook_manager, const std::string &source_notebook_id,
    const std::string &source_relative_path, const std::string &destination_notebook_id,
    const std::string &destination_folder_path, const nlohmann::json &options,
    const NodeTransferProgress &progress, std::unique_ptr<PreparedNodeTransfer> &out_transfer,
    std::string &out_error, Notebook *source_override, Notebook *destination_override) {
  out_transfer.reset();
  out_error.clear();
  if ((!notebook_manager && (!source_override || !destination_override)) ||
      (source_notebook_id == destination_notebook_id && !source_override) ||
      !IsSafeRelativePath(source_relative_path, false) ||
      !IsSafeRelativePath(destination_folder_path, true) || !options.is_object()) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  const std::string operation = options.value("operation", std::string());
  const std::string conflict_policy = options.value("conflictPolicy", std::string());
  const std::string timestamp_policy = options.value("timestampPolicy", std::string());
  if ((operation != "copy" && operation != "move") || conflict_policy != "rename" ||
      (timestamp_policy != "reset" && timestamp_policy != "preserve") ||
      !options.value("preserveRelativeLinks", true)) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  Notebook *source = source_override ? source_override : notebook_manager->GetNotebook(source_notebook_id);
  Notebook *destination = destination_override ? destination_override
      : notebook_manager->GetNotebook(destination_notebook_id);
  if (!source || !destination) {
    return VXCORE_ERR_NOT_FOUND;
  }
  if (source_override && operation != "copy") return VXCORE_ERR_INVALID_PARAM;
  if (source->GetType() != NotebookType::Bundled ||
      destination->GetType() != NotebookType::Bundled) {
    return VXCORE_ERR_UNSUPPORTED;
  }
  if (source->IsEncryptionRecoveryRequired()) {
    return VXCORE_ERR_ENCRYPTION_RECOVERY_REQUIRED;
  }
  if (destination->CheckWritable() != VXCORE_OK ||
      (operation == "move" && source->CheckWritable() != VXCORE_OK)) {
    return destination->CheckWritable() != VXCORE_OK ? destination->CheckWritable()
                                                     : source->CheckWritable();
  }
  auto *source_manager = dynamic_cast<BundledFolderManager *>(source->GetFolderManager());
  auto *destination_manager = dynamic_cast<BundledFolderManager *>(destination->GetFolderManager());
  if (!source_manager || !destination_manager) {
    return VXCORE_ERR_UNSUPPORTED;
  }
  const std::string clean_source = source->GetCleanRelativePath(source_relative_path);
  const std::string clean_destination = destination->GetCleanRelativePath(destination_folder_path);
  if (clean_source.empty() || clean_source == "." || clean_destination.empty()) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  FolderConfig *destination_parent = nullptr;
  VxCoreError error =
      destination_manager->TransferGetFolderConfig(clean_destination, &destination_parent);
  if (error != VXCORE_OK || !destination_parent ||
      !IsDirectory(destination->GetAbsolutePath(clean_destination))) {
    return error == VXCORE_OK ? VXCORE_ERR_NODE_NOT_EXISTS : error;
  }

  auto transfer = std::make_unique<PreparedNodeTransferImpl>();
  transfer->notebook_manager = notebook_manager;
  transfer->source_override = source_override;
  transfer->destination_override = destination_override;
  transfer->source_notebook_id = source_notebook_id;
  transfer->source_relative_path = clean_source;
  transfer->destination_notebook_id = destination_notebook_id;
  transfer->destination_folder_path = clean_destination;
  transfer->operation = operation;
  transfer->preserve_timestamps = timestamp_policy == "preserve";
  transfer->create_missing_tags = options.value("createMissingTags", true);
  const std::string configured_test_fault = options.value("testFault", std::string());
  if (!configured_test_fault.empty() && configured_test_fault != "postCommitVerification" &&
      configured_test_fault != "commitJournal" && configured_test_fault != "resultBeforeCommit" &&
      configured_test_fault != "resultAfterCommit" && configured_test_fault != "sourceRemoval" &&
      configured_test_fault != "sourceRollbackContent" &&
      configured_test_fault != "sourceRollbackParent" &&
      configured_test_fault != "sourceQuarantineException" &&
      configured_test_fault != "destinationPublicationRace") {
    return VXCORE_ERR_INVALID_PARAM;
  }
  transfer->test_fail_post_commit_verification =
      ConfigManager::IsTestMode() && configured_test_fault == "postCommitVerification";
  transfer->test_fail_commit_journal =
      ConfigManager::IsTestMode() && configured_test_fault == "commitJournal";
  transfer->test_fail_result_before_commit =
      ConfigManager::IsTestMode() && configured_test_fault == "resultBeforeCommit";
  transfer->test_fail_result_after_commit =
      ConfigManager::IsTestMode() && configured_test_fault == "resultAfterCommit";
  transfer->test_fail_source_removal =
      ConfigManager::IsTestMode() && configured_test_fault == "sourceRemoval";
  transfer->test_fail_source_rollback_content =
      ConfigManager::IsTestMode() && configured_test_fault == "sourceRollbackContent";
  transfer->test_fail_source_rollback_parent =
      ConfigManager::IsTestMode() && configured_test_fault == "sourceRollbackParent";
  transfer->test_throw_after_source_quarantine =
      ConfigManager::IsTestMode() && configured_test_fault == "sourceQuarantineException";
  transfer->test_destination_publication_race =
      ConfigManager::IsTestMode() && configured_test_fault == "destinationPublicationRace";
  transfer->source_name = SplitPath(clean_source).second;
  transfer->staging_dir = ConcatenatePaths(
      ConcatenatePaths(destination->GetMetadataFolder(), kTransferDirName), GenerateUUID());

  const FileRecord *source_file = nullptr;
  error = source_manager->GetFileInfo(clean_source, &source_file);
  std::unordered_map<std::string, std::string> ids;
  if (error == VXCORE_OK && source_file) {
    transfer->is_folder = false;
    if (!IsRegularFile(source->GetAbsolutePath(clean_source))) {
      return VXCORE_ERR_NODE_NOT_EXISTS;
    }
    error = ValidateAttachments(*source_file);
    if (error != VXCORE_OK) {
      return error;
    }
    IndexedFile file;
    file.relative_path = source_file->name;
    file.source_record = *source_file;
    file.destination_record = *source_file;
    error = ResolveAssetsRoot(source, clean_source, file.source_assets_root);
    if (error != VXCORE_OK) {
      return error;
    }
    ids[source_file->id] = GenerateUUID();
    transfer->files.push_back(std::move(file));
  } else {
    FolderConfig *source_folder = nullptr;
    error = source_manager->TransferGetFolderConfig(clean_source, &source_folder);
    if (error != VXCORE_OK || !source_folder) {
      return error;
    }
    if (!IsDirectory(source->GetAbsolutePath(clean_source))) {
      return VXCORE_ERR_NODE_NOT_EXISTS;
    }
    if (source == destination &&
        IsPathWithin(source->GetAbsolutePath(clean_source),
                     destination->GetAbsolutePath(clean_destination), false)) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    transfer->is_folder = true;
    error = CollectFolder(source_manager, source, *transfer, clean_source, ".", ids);
    if (error != VXCORE_OK) {
      return error;
    }
  }
  ApplyIdentities(*transfer, ids, GetCurrentTimestampMillis());
  for (const auto &file : transfer->files) {
    if (file.source_record.CheckProtectionMetadata() == VXCORE_OK) continue;
    if (!source->GetEncryption() || !destination->GetEncryption()) {
      return VXCORE_ERR_ENCRYPTION_LOCKED;
    }
    transfer->protected_keys = std::make_unique<ProtectedTransferKeys>();
    auto &keys = *transfer->protected_keys;
    error = source->GetEncryption()->AcquireNotebookKey(keys.source, &keys.source_envelope);
    if (error == VXCORE_OK) {
      error = destination->GetEncryption()->AcquireNotebookKey(
          keys.destination, &keys.destination_envelope);
    }
    if (error != VXCORE_OK) return error;
    error = ValidateTransferEnvelope(source, keys.source_envelope);
    if (error == VXCORE_OK) {
      error = ValidateTransferEnvelope(destination, keys.destination_envelope);
    }
    if (error != VXCORE_OK) return error;
    break;
  }

  std::set<std::string> tag_paths;
  for (const auto &file : transfer->files) {
    for (const auto &tag : file.source_record.tags) {
      const std::string path = TagPath(source->GetConfig(), tag);
      if (!path.empty()) {
        tag_paths.insert(path);
      }
    }
  }
  transfer->tag_paths.assign(tag_paths.begin(), tag_paths.end());

  error = HashSource(*transfer, transfer->source_fingerprint);
  if (error != VXCORE_OK) {
    return error;
  }
  if (progress && !progress("enumerate", 0, 0)) {
    return VXCORE_ERR_CANCELLED;
  }

  const fs::path staging = PathFromUtf8(transfer->staging_dir);
  const fs::path staged_content = staging / kContentName;
  std::set<std::string> excluded;
  for (const auto &file : transfer->files) {
    excluded.insert(CleanPath(file.source_assets_root));
  }
  uint64_t total = CountBytes(PathFromUtf8(source->GetAbsolutePath(clean_source)));
  for (const auto &file : transfer->files) {
    const std::string assets = ConcatenatePaths(file.source_assets_root, file.source_record.id);
    if (PathExists(assets)) {
      total += CountBytes(PathFromUtf8(assets));
    }
  }
  uint64_t completed = 0;
  error = CopyTree(PathFromUtf8(source->GetAbsolutePath(clean_source)), staged_content, excluded,
                   progress, completed, total);
  if (error != VXCORE_OK) {
    RemoveQuietly(staging);
    return error;
  }
  if (transfer->is_folder) {
    error = WriteStagedMetadata(*transfer);
    if (error != VXCORE_OK) {
      RemoveQuietly(staging);
      return error;
    }
  }
  for (size_t i = 0; i < transfer->files.size(); ++i) {
    auto &file = transfer->files[i];
    const std::string source_assets =
        ConcatenatePaths(file.source_assets_root, file.source_record.id);
    if (PathExists(source_assets)) {
      file.staged_assets_root = PathToUtf8(staging / kAssetsName / std::to_string(i));
      error = CopyTree(PathFromUtf8(source_assets), PathFromUtf8(file.staged_assets_root), {},
                       progress, completed, total);
      if (error != VXCORE_OK) {
        RemoveQuietly(staging);
        return error;
      }
    }
    if (file.source_record.CheckProtectionMetadata() != VXCORE_OK) {
      const auto &keys = *transfer->protected_keys;
      const auto source_path = transfer->is_folder
          ? ConcatenatePaths(clean_source, file.relative_path) : clean_source;
      const auto target_path = transfer->is_folder
          ? staged_content / PathFromUtf8(file.relative_path) : staged_content;
      const auto backup_target = transfer->is_folder
          ? PathFromUtf8(PathToUtf8(target_path) + ".vswp")
          : staging / kAssetsName / std::to_string(transfer->files.size() + i);
      ContentProcessor processor;
      const auto old_assets = ConcatenatePaths(source->GetConfig().assets_folder, file.source_record.id);
      const auto new_assets = ConcatenatePaths(destination->GetConfig().assets_folder, file.destination_record.id);
      const auto rewrite_body = [&](std::vector<uint8_t> &body, const std::string &editor) {
        auto *handler = processor.GetHandler(editor == "markdown" ? "md" : "");
        if (!handler || body.empty() || old_assets == new_assets) return false;
        struct WipedText {
          std::string text;
          std::string rewritten;
          ~WipedText() {
            NotebookEncryption::WipeString(text);
            NotebookEncryption::WipeString(rewritten);
          }
        } plaintext;
        plaintext.text.assign(reinterpret_cast<const char *>(body.data()), body.size());
        plaintext.rewritten = handler->RewriteAssetLinks(plaintext.text, old_assets, new_assets);
        if (plaintext.rewritten == plaintext.text) return false;
        NotebookEncryption::WipeBytes(body);
        body.assign(plaintext.rewritten.begin(), plaintext.rewritten.end());
        return true;
      };
      error = NotebookEncryption::TransferNote(
          PathFromUtf8(source->GetAbsolutePath(source_path)),
          keys.source_envelope, *keys.source, target_path, keys.destination_envelope,
          *keys.destination, operation == "copy", backup_target, file.has_backup, rewrite_body);
      if (error != VXCORE_OK) {
        RemoveQuietly(staging);
        return error;
      }
      continue;
    }
    const fs::path staged_file =
        transfer->is_folder ? staged_content / PathFromUtf8(file.relative_path) : staged_content;
    ContentProcessor processor;
    if (IFileTypeHandler *handler = processor.GetHandler(Extension(file.source_record.name))) {
      std::string content;
      if (ReadFile(staged_file, content) != VXCORE_OK) {
        RemoveQuietly(staging);
        return VXCORE_ERR_IO;
      }
      const std::string old_assets =
          ConcatenatePaths(source->GetConfig().assets_folder, file.source_record.id);
      const std::string new_assets =
          ConcatenatePaths(destination->GetConfig().assets_folder, file.destination_record.id);
      const std::string rewritten = handler->RewriteAssetLinks(content, old_assets, new_assets);
      if (rewritten != content && WriteFile(staged_file, rewritten) != VXCORE_OK) {
        RemoveQuietly(staging);
        return VXCORE_ERR_IO;
      }
    }
  }
  std::string verified;
  error = HashSource(*transfer, verified);
  if (error != VXCORE_OK || verified != transfer->source_fingerprint) {
    RemoveQuietly(staging);
    return error == VXCORE_OK ? VXCORE_ERR_INVALID_STATE : error;
  }
  if (progress && !progress("verify", total, total)) {
    RemoveQuietly(staging);
    return VXCORE_ERR_CANCELLED;
  }
  if (!WriteDurable(staging / kManifestName, ToManifest(*transfer).dump(2))) {
    RemoveQuietly(staging);
    return VXCORE_ERR_IO;
  }
  out_transfer = std::move(transfer);
  return VXCORE_OK;
}

VxCoreError NodeTransfer::Commit(NotebookManager *notebook_manager,
                                 std::unique_ptr<PreparedNodeTransfer> transfer_handle,
                                 const std::string &event_batch_id,
                                 NodeTransferCommitResult &out_result, nlohmann::json &out_events,
                                 std::string &out_error) {
  if (event_batch_id.empty()) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  out_result.Reset();
  out_events = nlohmann::json::array();
  out_error.clear();
  std::unique_ptr<PreparedNodeTransferImpl> prepared(
      static_cast<PreparedNodeTransferImpl *>(transfer_handle.release()));
  if (!prepared || prepared->notebook_manager != notebook_manager) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  PreparedNodeTransferImpl *transfer = prepared.get();
  Notebook *source = transfer->source_override ? transfer->source_override
      : notebook_manager->GetNotebook(transfer->source_notebook_id);
  Notebook *destination = transfer->destination_override ? transfer->destination_override
      : notebook_manager->GetNotebook(transfer->destination_notebook_id);
  if (!source || !destination) {
    Discard(std::move(prepared));
    return VXCORE_ERR_NOT_FOUND;
  }
  if (transfer->protected_keys) {
    auto error = ValidateTransferEnvelope(source, transfer->protected_keys->source_envelope);
    if (error == VXCORE_OK) {
      error = ValidateTransferEnvelope(destination, transfer->protected_keys->destination_envelope);
    }
    if (error != VXCORE_OK) {
      Discard(std::move(prepared));
      return error;
    }
  }
  if (source->IsEncryptionRecoveryRequired() || destination->CheckWritable() != VXCORE_OK ||
      (transfer->operation == "move" && source->CheckWritable() != VXCORE_OK)) {
    const auto error = source->IsEncryptionRecoveryRequired()
                           ? VXCORE_ERR_ENCRYPTION_RECOVERY_REQUIRED
                           : (destination->CheckWritable() != VXCORE_OK
                                  ? destination->CheckWritable() : source->CheckWritable());
    Discard(std::move(prepared));
    return error;
  }
  auto *manager = dynamic_cast<BundledFolderManager *>(destination->GetFolderManager());
  if (!manager) {
    Discard(std::move(prepared));
    return VXCORE_ERR_UNSUPPORTED;
  }
  FolderConfig *parent = nullptr;
  VxCoreError error = manager->TransferGetFolderConfig(transfer->destination_folder_path, &parent);
  if (error != VXCORE_OK || !parent) {
    Discard(std::move(prepared));
    return error;
  }
  std::string current_source_fingerprint;
  error = HashSource(*transfer, current_source_fingerprint);
  if (error != VXCORE_OK || current_source_fingerprint != transfer->source_fingerprint) {
    out_error = "Source changed after the transfer snapshot";
    Discard(std::move(prepared));
    return error == VXCORE_OK ? VXCORE_ERR_INVALID_STATE : error;
  }
  const FolderConfig original_parent = *parent;
  std::string name;
  error = SelectDestinationName(manager, transfer->destination_folder_path, transfer->source_name,
                                transfer->is_folder, name);
  if (error != VXCORE_OK) {
    Discard(std::move(prepared));
    return error;
  }
  const std::string destination_path = ConcatenatePaths(transfer->destination_folder_path, name);
  const fs::path staging = PathFromUtf8(transfer->staging_dir);
  const fs::path content_target = PathFromUtf8(destination->GetAbsolutePath(destination_path));
  const fs::path metadata_target =
      PathFromUtf8(manager->TransferGetConfigPath(destination_path)).parent_path();
  error = ValidateContainedPath(destination, content_target);
  if (error == VXCORE_OK) {
    error = ValidateContainedPath(destination, metadata_target);
  }
  if (error != VXCORE_OK) {
    Discard(std::move(prepared));
    return error;
  }
  if (transfer->is_folder) {
    transfer->folders.front().destination_config.name = name;
    error = WriteStagedMetadata(*transfer);
    if (error != VXCORE_OK) {
      Discard(std::move(prepared));
      return error;
    }
  }
  std::string destination_fingerprint;
  error = FingerprintStagedDestination(*transfer, name, destination_fingerprint);
  if (error != VXCORE_OK) {
    Discard(std::move(prepared));
    return error;
  }

  NotebookConfig updated_notebook_config = destination->GetConfig();
  std::vector<TagNode> created_tags;
  std::vector<std::string> created_tag_paths;
  if (transfer->create_missing_tags) {
    std::vector<std::string> paths = transfer->tag_paths;
    std::sort(paths.begin(), paths.end(), [](const std::string &left, const std::string &right) {
      return SplitPathComponents(left).size() < SplitPathComponents(right).size();
    });
    for (const auto &path : paths) {
      std::string parent_name;
      for (const auto &component : SplitPathComponents(path)) {
        auto existing =
            std::find_if(updated_notebook_config.tags.begin(), updated_notebook_config.tags.end(),
                         [&](const TagNode &tag) { return tag.name == component; });
        if (existing == updated_notebook_config.tags.end()) {
          TagNode tag(component, parent_name);
          updated_notebook_config.tags.push_back(tag);
          created_tags.push_back(tag);
          created_tag_paths.push_back(parent_name.empty() ? component
                                                          : parent_name + "/" + component);
        }
        parent_name = component;
      }
    }
    if (!created_tags.empty()) {
      updated_notebook_config.tags_modified_utc = GetCurrentTimestampMillis();
    }
  }

  nlohmann::json asset_targets = nlohmann::json::array();
  for (size_t i = 0; i < transfer->files.size(); ++i) {
    const auto &file = transfer->files[i];
    const std::string file_path = transfer->is_folder
                                      ? ConcatenatePaths(destination_path, file.relative_path)
                                      : destination_path;
    std::string assets_root;
    error = ResolveAssetsRoot(destination, file_path, assets_root);
    if (error != VXCORE_OK) {
      Discard(std::move(prepared));
      return error;
    }
    if (!file.staged_assets_root.empty()) {
    const fs::path target = PathFromUtf8(ConcatenatePaths(assets_root, file.destination_record.id));
    std::error_code ec;
    if (fs::exists(target, ec) || ec) {
      Discard(std::move(prepared));
      return ec ? VXCORE_ERR_IO : VXCORE_ERR_ALREADY_EXISTS;
    }
    asset_targets.push_back({{"staged", file.staged_assets_root},
                             {"target", PathToUtf8(target)},
                             {"relativePath", file.relative_path},
                             {"filePath", file_path},
                             {"id", file.destination_record.id},
                             {"slot", std::to_string(i)}});
    }
    if (!transfer->is_folder && file.has_backup) {
      const auto slot = std::to_string(transfer->files.size() + i);
      const auto backup = PathFromUtf8(destination->GetAbsolutePath(file_path) + ".vswp");
      if (PathExists(PathToUtf8(backup))) {
        Discard(std::move(prepared));
        return VXCORE_ERR_ALREADY_EXISTS;
      }
      asset_targets.push_back({{"staged", PathToUtf8(staging / kAssetsName / slot)},
          {"target", PathToUtf8(backup)}, {"relativePath", file.relative_path},
          {"filePath", file_path}, {"id", file.destination_record.id}, {"slot", slot},
          {"backup", true}});
    }
  }

  const fs::path journal_path = staging / kJournalName;
  const fs::path parent_config_path =
      PathFromUtf8(manager->TransferGetConfigPath(transfer->destination_folder_path));
  const fs::path notebook_config_path =
      PathFromUtf8(destination->GetMetadataFolder()) / "config.json";
  std::string old_parent_bytes;
  std::string old_notebook_bytes;
  if (ReadFile(parent_config_path, old_parent_bytes) != VXCORE_OK ||
      ReadFile(notebook_config_path, old_notebook_bytes) != VXCORE_OK) {
    Discard(std::move(prepared));
    return VXCORE_ERR_IO;
  }
  FolderConfig updated_parent = original_parent;
  if (transfer->is_folder) {
    updated_parent.folders.push_back(name);
  } else {
    FileRecord record = transfer->files.front().destination_record;
    record.name = name;
    updated_parent.files.push_back(record);
  }
  updated_parent.modified_utc = GetCurrentTimestampMillis();
  const std::string committed_parent_bytes = updated_parent.ToJson().dump(2);
  nlohmann::json journal = {
      {"kind", kKindDestination},
      {"phase", kPhaseInit},
      {"destinationPath", destination_path},
      {"destinationParentPath", transfer->destination_folder_path},
      {"contentTarget", PathToUtf8(content_target)},
      {"metadataTarget", transfer->is_folder ? PathToUtf8(metadata_target) : ""},
      {"isFolder", transfer->is_folder},
      {"parentConfigBytes", old_parent_bytes},
      {"committedParentConfigBytes", committed_parent_bytes},
      {"notebookConfigBytes", old_notebook_bytes},
      {"createdTags", nlohmann::json::array()},
      {"destinationFingerprint", destination_fingerprint},
      {"assets", asset_targets},
      {"ids", nlohmann::json::array()}};
  for (const auto &file : transfer->files) {
    journal["ids"].push_back(file.destination_record.id);
  }
  for (const auto &folder : transfer->folders) {
    journal["ids"].push_back(folder.destination_config.id);
  }
  for (const auto &tag : created_tags) {
    journal["createdTags"].push_back(tag.name);
  }

  nlohmann::json destination_events = nlohmann::json::array();
  if (!created_tags.empty()) {
    AppendEvent(destination_events, events::kNotebookConfigChanged,
                {{kJsonKeyNotebookId, destination->GetId()}});
  }
  AppendEvent(
      destination_events, events::kFolderConfigChanged,
      {{kJsonKeyNotebookId, destination->GetId()}, {"path", transfer->destination_folder_path}});
  AppendEvent(destination_events,
              transfer->is_folder ? events::kFolderCreated : events::kFileCreated,
              {{kJsonKeyNotebookId, destination->GetId()}, {"path", destination_path}});
  nlohmann::json moved_events = destination_events;
  AppendEvent(moved_events, events::kFolderConfigChanged,
              {{kJsonKeyNotebookId, source->GetId()},
               {"path", SplitPath(transfer->source_relative_path).first}});
  AppendEvent(moved_events, transfer->is_folder ? events::kFolderDeleted : events::kFileDeleted,
              {{kJsonKeyNotebookId, source->GetId()}, {"path", transfer->source_relative_path}});

  nlohmann::json base_result = {{"status", transfer->operation == "move" ? "moved" : "copied"},
                                {"sourceNotebookId", transfer->source_notebook_id},
                                {"sourceRelativePath", transfer->source_relative_path},
                                {"destinationNotebookId", transfer->destination_notebook_id},
                                {"destinationRelativePath", destination_path},
                                {"destinationNodeId", transfer->destination_node_id},
                                {"isFolder", transfer->is_folder},
                                {"createdTagPaths", created_tag_paths}};
  nlohmann::json deferred_result = base_result;
  deferred_result["recoveryDeferred"] = true;
  nlohmann::json retained_result;
  nlohmann::json recovery_required_result;
  if (transfer->operation == "move") {
    transfer->source_recovery_id = "remove-" + GenerateUUID();
    const nlohmann::json resume_token = {
        {"sourceNotebookId", transfer->source_notebook_id},
        {"sourceRelativePath", transfer->source_relative_path},
        {"destinationNotebookId", transfer->destination_notebook_id},
        {"destinationRelativePath", destination_path},
        {"destinationNodeId", transfer->destination_node_id},
        {"isFolder", transfer->is_folder},
        {"sourceFingerprint", transfer->source_fingerprint},
        {"sourceRecoveryId", transfer->source_recovery_id},
        {"destinationFingerprint", destination_fingerprint},
        {"files", ToManifest(*transfer)["files"]},
        {"folders", ToManifest(*transfer)["folders"]}};
    retained_result = base_result;
    retained_result["status"] = "copiedSourceRetained";
    retained_result["resumeToken"] = resume_token;
    recovery_required_result = base_result;
    recovery_required_result["status"] = "moveRecoveryRequired";
    recovery_required_result["recoveryRequired"] = true;
    recovery_required_result["recoveryDeferred"] = true;
    recovery_required_result["resumeToken"] = resume_token;
  }
  PreparedCString success_json = PrepareResultString(base_result, event_batch_id);
  PreparedCString retained_json;
  PreparedCString recovery_json = PrepareResultString(
      transfer->operation == "move" ? recovery_required_result : deferred_result, event_batch_id);
  if (transfer->operation == "move") {
    retained_json = PrepareResultString(retained_result, event_batch_id);
  }
  if (!success_json || !recovery_json || (transfer->operation == "move" && !retained_json) ||
      transfer->test_fail_result_before_commit) {
    Discard(std::move(prepared));
    return VXCORE_ERR_OUT_OF_MEMORY;
  }
  out_result.success_json = success_json.release();
  out_result.retained_json = retained_json.release();
  out_result.recovery_json = recovery_json.release();
  auto save_phase = [&](const char *phase) {
    if (transfer->test_fail_commit_journal && phase == kPhaseCommitted) {
      return false;
    }
    journal["phase"] = phase;
    return WriteDurable(journal_path, journal.dump(2));
  };
  if (!save_phase(kPhaseInit)) {
    Discard(std::move(prepared));
    return VXCORE_ERR_IO;
  }

  bool notebook_config_written = false;
  bool content_published = false;
  bool metadata_published = false;
  std::vector<fs::path> published_assets;
  bool db_written = false;
  bool parent_config_written = false;
  auto rollback = [&]() -> VxCoreError {
    VxCoreError rollback_error = VXCORE_OK;
    if (db_written) {
      rollback_error = DeleteStoreIds(destination, *transfer, created_tags);
    }
    for (auto it = published_assets.rbegin(); it != published_assets.rend(); ++it) {
      if (!RemovePathChecked(*it)) {
        rollback_error = VXCORE_ERR_IO;
      }
    }
    if (metadata_published && !RemovePathChecked(metadata_target)) {
      rollback_error = VXCORE_ERR_IO;
    }
    if (content_published && !RemovePathChecked(content_target)) {
      rollback_error = VXCORE_ERR_IO;
    }
    if (parent_config_written && !ReplaceDurable(parent_config_path, old_parent_bytes)) {
      rollback_error = VXCORE_ERR_IO;
    }
    if (notebook_config_written && !ReplaceDurable(notebook_config_path, old_notebook_bytes)) {
      rollback_error = VXCORE_ERR_IO;
    }
    manager->TransferInvalidateCache(transfer->destination_folder_path);
    if (rollback_error == VXCORE_OK && !CleanupJournalDir(staging)) {
      rollback_error = VXCORE_ERR_IO;
    }
    return rollback_error;
  };
  if (!created_tags.empty() &&
      !ReplaceDurable(notebook_config_path, updated_notebook_config.ToJson().dump(2))) {
    const VxCoreError rollback_error = rollback();
    return rollback_error == VXCORE_OK ? VXCORE_ERR_IO : rollback_error;
  }
  notebook_config_written = !created_tags.empty();
  if (transfer->test_destination_publication_race) {
    if (transfer->is_folder) {
      std::error_code race_error;
      fs::create_directories(content_target, race_error);
      if (!race_error) {
        WriteDurable(content_target / "external.txt", "external contender\n");
      }
    } else {
      WriteDurable(content_target, "external contender\n");
    }
  }
  const PublishResult content_publish =
      PublishPathNoReplace(staging / kContentName, content_target);
  if (content_publish != PublishResult::Published) {
    const VxCoreError rollback_error = rollback();
    return rollback_error == VXCORE_OK
               ? (content_publish == PublishResult::Collision ? VXCORE_ERR_ALREADY_EXISTS
                                                              : VXCORE_ERR_IO)
               : rollback_error;
  }
  content_published = true;
  if (transfer->is_folder &&
      PublishPathNoReplace(staging / kMetadataName, metadata_target) != PublishResult::Published) {
    const VxCoreError rollback_error = rollback();
    return rollback_error == VXCORE_OK ? VXCORE_ERR_IO : rollback_error;
  }
  metadata_published = transfer->is_folder;
  for (const auto &asset : asset_targets) {
    const fs::path target = PathFromUtf8(asset["target"].get<std::string>());
    if (PublishPathNoReplace(PathFromUtf8(asset["staged"].get<std::string>()), target) !=
        PublishResult::Published) {
      const VxCoreError rollback_error = rollback();
      return rollback_error == VXCORE_OK ? VXCORE_ERR_IO : rollback_error;
    }
    published_assets.push_back(target);
  }
  if (!save_phase(kPhasePublished)) {
    const VxCoreError rollback_error = rollback();
    return rollback_error == VXCORE_OK ? VXCORE_ERR_IO : rollback_error;
  }
  error = InsertDestinationStore(destination, *transfer, name, created_tags, parent->id);
  if (error != VXCORE_OK) {
    const VxCoreError rollback_error = rollback();
    return rollback_error == VXCORE_OK ? error : rollback_error;
  }
  db_written = true;
  if (!save_phase(kPhaseDb)) {
    const VxCoreError rollback_error = rollback();
    return rollback_error == VXCORE_OK ? VXCORE_ERR_IO : rollback_error;
  }

  if (!ReplaceDurable(parent_config_path, committed_parent_bytes)) {
    const VxCoreError rollback_error = rollback();
    return rollback_error == VXCORE_OK ? VXCORE_ERR_IO : rollback_error;
  }
  parent_config_written = true;
  out_events.swap(destination_events);
  out_result.outcome = NodeTransferCommitOutcome::DestinationCommitted;
  bool recovery_deferred = false;
  if (!save_phase(kPhaseCommitted)) {
    recovery_deferred = true;
    VXCORE_LOG_ERROR(
        "NodeTransfer: destination parent commit is durable but journal advancement failed; "
        "returning a durable fact and retaining recovery state");
  }
  if (notebook_config_written) {
    error = ReloadNotebookConfig(destination, notebook_config_path);
    if (error != VXCORE_OK) {
      VXCORE_LOG_ERROR(
          "NodeTransfer: destination committed but live config reload failed (error=%d); "
          "journal retained for open-time cleanup",
          error);
      recovery_deferred = true;
    }
  }
  manager->TransferInvalidateCache(destination_path);
  manager->TransferInvalidateCache(transfer->destination_folder_path);

  std::string verified_destination_fingerprint;
  error = transfer->test_fail_post_commit_verification
              ? VXCORE_ERR_IO
              : FingerprintDestination(destination, manager, destination_path, transfer->is_folder,
                                       transfer->files, verified_destination_fingerprint);
  if (error == VXCORE_OK && verified_destination_fingerprint != destination_fingerprint) {
    error = VXCORE_ERR_INVALID_STATE;
  }
  if (error == VXCORE_OK && !recovery_deferred && !CleanupJournalDir(staging)) {
    VXCORE_LOG_WARN("NodeTransfer: committed destination cleanup deferred to recovery: %s",
                    transfer->staging_dir.c_str());
  }
  if (transfer->test_fail_result_after_commit) {
    throw std::bad_alloc();
  }

  if (transfer->operation == "move") {
    const bool destination_verified = error == VXCORE_OK;
    if (destination_verified && !recovery_deferred) {
      const SourceRemovalOutcome removal = RemoveSource(*transfer, out_error);
      if (removal.state == SourceRemovalState::RecoveryRequired) {
        out_result.outcome = NodeTransferCommitOutcome::RecoveryRequired;
        return VXCORE_OK;
      }
      if (removal.state == SourceRemovalState::RestoredSourceRetained) {
        out_result.outcome = NodeTransferCommitOutcome::SourceRetained;
        return VXCORE_OK;
      }
    } else {
      out_error = "Destination committed, but recovery is required before source removal";
      out_result.outcome = NodeTransferCommitOutcome::RecoveryRequired;
      return VXCORE_OK;
    }
    out_result.outcome = NodeTransferCommitOutcome::Success;
    out_events.swap(moved_events);
  } else if (!recovery_deferred && error == VXCORE_OK &&
             verified_destination_fingerprint == destination_fingerprint) {
    out_result.outcome = NodeTransferCommitOutcome::Success;
  } else {
    out_result.outcome = NodeTransferCommitOutcome::RecoveryRequired;
  }
  return VXCORE_OK;
}

VxCoreError NodeTransfer::FinalizeMove(NotebookManager *notebook_manager,
                                       const nlohmann::json &resume_token,
                                       const std::string &event_batch_id, char **out_result_json,
                                       nlohmann::json &out_events, std::string &out_error) {
  if (!out_result_json || event_batch_id.empty()) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  *out_result_json = nullptr;
  out_events = nlohmann::json::array();
  out_error.clear();
  if (!notebook_manager || !resume_token.is_object()) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  auto transfer = std::make_unique<PreparedNodeTransferImpl>();
  transfer->notebook_manager = notebook_manager;
  transfer->source_notebook_id = resume_token.value("sourceNotebookId", std::string());
  transfer->source_relative_path = resume_token.value("sourceRelativePath", std::string());
  transfer->destination_notebook_id = resume_token.value("destinationNotebookId", std::string());
  const std::string destination_path = resume_token.value("destinationRelativePath", std::string());
  transfer->destination_node_id = resume_token.value("destinationNodeId", std::string());
  transfer->source_fingerprint = resume_token.value("sourceFingerprint", std::string());
  transfer->source_recovery_id = resume_token.value("sourceRecoveryId", std::string());
  transfer->is_folder = resume_token.value("isFolder", false);
  transfer->operation = "move";
  if (transfer->source_notebook_id.empty() || transfer->destination_notebook_id.empty() ||
      transfer->source_relative_path.empty() || destination_path.empty() ||
      transfer->source_fingerprint.empty() || transfer->source_recovery_id.size() != 43 ||
      transfer->source_recovery_id.rfind("remove-", 0) != 0 ||
      !std::all_of(transfer->source_recovery_id.begin() + 7, transfer->source_recovery_id.end(),
                   [](unsigned char ch) { return std::isxdigit(ch) != 0 || ch == '-'; }) ||
      !resume_token.contains("files") || !resume_token["files"].is_array() ||
      !resume_token.contains("folders") || !resume_token["folders"].is_array()) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  for (const auto &entry : resume_token["files"]) {
    IndexedFile file;
    file.relative_path = entry.value("relativePath", std::string());
    file.source_record = FileRecord::FromJson(entry["source"]);
    file.destination_record = FileRecord::FromJson(entry["destination"]);
    file.source_assets_root = entry.value("sourceAssetsRoot", std::string());
    if (!IsSafeRelativePath(file.relative_path, false) || file.source_record.id.empty() ||
        file.destination_record.id.empty()) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    transfer->files.push_back(std::move(file));
  }
  for (const auto &entry : resume_token["folders"]) {
    if (!entry.is_object() || !entry.contains("source")) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    IndexedFolder folder;
    folder.relative_path = entry.value("relativePath", std::string());
    folder.source_config = FolderConfig::FromJson(entry["source"]);
    if (!IsSafeRelativePath(folder.relative_path, true) || folder.source_config.id.empty()) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    transfer->folders.push_back(std::move(folder));
  }
  Notebook *source = notebook_manager->GetNotebook(transfer->source_notebook_id);
  Notebook *destination = notebook_manager->GetNotebook(transfer->destination_notebook_id);
  if (!source || !destination) {
    return VXCORE_ERR_NOT_FOUND;
  }
  auto *source_manager = dynamic_cast<BundledFolderManager *>(source->GetFolderManager());
  auto *destination_manager = dynamic_cast<BundledFolderManager *>(destination->GetFolderManager());
  if (!source_manager || !destination_manager) {
    return VXCORE_ERR_UNSUPPORTED;
  }
  for (const auto &file : transfer->files) {
    const std::string source_file_path =
        transfer->is_folder ? ConcatenatePaths(transfer->source_relative_path, file.relative_path)
                            : transfer->source_relative_path;
    std::string expected_assets_root;
    if (ResolveAssetsRoot(source, source_file_path, expected_assets_root) != VXCORE_OK ||
        !CanonicallyEqual(PathFromUtf8(file.source_assets_root),
                          PathFromUtf8(expected_assets_root))) {
      return VXCORE_ERR_INVALID_PARAM;
    }
  }
  if (transfer->is_folder && transfer->folders.empty()) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  std::string source_fingerprint;
  VxCoreError error = HashSource(*transfer, source_fingerprint);
  if (error != VXCORE_OK || source_fingerprint != transfer->source_fingerprint) {
    const fs::path recovery_journal = PathFromUtf8(source->GetMetadataFolder()) / kTransferDirName /
                                      PathFromUtf8(transfer->source_recovery_id) / kJournalName;
    bool journal_exists = false;
    if (!PathExistsChecked(recovery_journal, journal_exists)) {
      return VXCORE_ERR_IO;
    }
    FolderConfig *parent = nullptr;
    const auto split = SplitPath(transfer->source_relative_path);
    const VxCoreError parent_error = source_manager->TransferGetFolderConfig(split.first, &parent);
    const bool source_reachable =
        parent_error == VXCORE_OK && parent &&
        (transfer->is_folder
             ? std::find(parent->folders.begin(), parent->folders.end(), split.second) !=
                   parent->folders.end()
             : std::any_of(parent->files.begin(), parent->files.end(), [&](const FileRecord &file) {
                 return !transfer->files.empty() &&
                        file.id == transfer->files.front().source_record.id;
               }));
    if (journal_exists || source_reachable) {
      return error == VXCORE_OK ? VXCORE_ERR_INVALID_STATE : error;
    }
    error = FingerprintDestination(destination, destination_manager, destination_path,
                                   transfer->is_folder, transfer->files, source_fingerprint);
    if (error != VXCORE_OK ||
        source_fingerprint != resume_token.value("destinationFingerprint", std::string())) {
      return error == VXCORE_OK ? VXCORE_ERR_INVALID_STATE : error;
    }
    nlohmann::json result = {{"status", "moved"},
                             {"sourceNotebookId", transfer->source_notebook_id},
                             {"sourceRelativePath", transfer->source_relative_path},
                             {"destinationNotebookId", transfer->destination_notebook_id},
                             {"destinationRelativePath", destination_path},
                             {"destinationNodeId", transfer->destination_node_id},
                             {"isFolder", transfer->is_folder},
                             {"createdTagPaths", nlohmann::json::array()}};
    PreparedCString result_json = PrepareResultString(std::move(result), event_batch_id);
    if (!result_json) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }
    SelectPreparedResult(out_result_json, result_json);
    return VXCORE_OK;
  }
  std::string destination_fingerprint;
  error = FingerprintDestination(destination, destination_manager, destination_path,
                                 transfer->is_folder, transfer->files, destination_fingerprint);
  if (error != VXCORE_OK ||
      destination_fingerprint != resume_token.value("destinationFingerprint", std::string())) {
    return error == VXCORE_OK ? VXCORE_ERR_INVALID_STATE : error;
  }
  nlohmann::json result = {{"status", "moved"},
                           {"sourceNotebookId", transfer->source_notebook_id},
                           {"sourceRelativePath", transfer->source_relative_path},
                           {"destinationNotebookId", transfer->destination_notebook_id},
                           {"destinationRelativePath", destination_path},
                           {"destinationNodeId", transfer->destination_node_id},
                           {"isFolder", transfer->is_folder},
                           {"createdTagPaths", nlohmann::json::array()}};
  PreparedCString result_json = PrepareResultString(std::move(result), event_batch_id);
  if (!result_json) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  }
  nlohmann::json prepared_events = nlohmann::json::array();
  AppendEvent(prepared_events, events::kFolderConfigChanged,
              {{kJsonKeyNotebookId, source->GetId()},
               {"path", SplitPath(transfer->source_relative_path).first}});
  AppendEvent(prepared_events, transfer->is_folder ? events::kFolderDeleted : events::kFileDeleted,
              {{kJsonKeyNotebookId, source->GetId()}, {"path", transfer->source_relative_path}});
  const SourceRemovalOutcome removal = RemoveSource(*transfer, out_error);
  if (removal.state != SourceRemovalState::Removed) {
    return removal.error;
  }
  SelectPreparedResult(out_result_json, result_json);
  out_events.swap(prepared_events);
  return VXCORE_OK;
}

VxCoreError NodeTransfer::Recover(Notebook *notebook, int *out_recovered_count) {
  if (out_recovered_count) {
    *out_recovered_count = 0;
  }
  if (!notebook || notebook->GetType() != NotebookType::Bundled) {
    return VXCORE_ERR_UNSUPPORTED;
  }
  auto *manager = dynamic_cast<BundledFolderManager *>(notebook->GetFolderManager());
  if (!manager) {
    return VXCORE_ERR_INVALID_STATE;
  }
  const fs::path root = PathFromUtf8(notebook->GetMetadataFolder()) / kTransferDirName;
  std::error_code ec;
  const fs::file_status root_status = fs::symlink_status(root, ec);
  if (ec == std::errc::no_such_file_or_directory ||
      root_status.type() == fs::file_type::not_found) {
    return VXCORE_OK;
  }
  if (ec || !fs::is_directory(root_status)) {
    return VXCORE_ERR_IO;
  }
  std::vector<fs::path> entries;
  int encryption_recovered = 0;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().filename() == fs::path("encryption")) {
      const auto error = manager->RecoverEncryptionTransactions(it->path(), &encryption_recovered);
      if (error != VXCORE_OK) {
        return error;
      }
      continue;
    }
    if (fs::is_directory(it->path(), ec)) {
      entries.push_back(it->path());
    }
  }
  if (ec) {
    return VXCORE_ERR_IO;
  }
  std::vector<nlohmann::json> journals(entries.size());
  std::vector<bool> destination_committed(entries.size(), false);
  for (size_t i = 0; i < entries.size(); ++i) {
    const fs::path &entry = entries[i];
    if (!IsPathWithin(PathToUtf8(root), PathToUtf8(entry), false) ||
        CheckReparsePoint(PathToUtf8(entry)) != ReparseState::kNo) {
      return VXCORE_ERR_INVALID_STATE;
    }
    const fs::path journal_path = entry / kJournalName;
    bool journal_exists = false;
    if (!PathExistsChecked(journal_path, journal_exists)) {
      return VXCORE_ERR_IO;
    }
    if (!journal_exists) {
      continue;
    }
    if (LoadJsonFile(journal_path, journals[i]) != VXCORE_OK || !journals[i].is_object()) {
      return VXCORE_ERR_INVALID_STATE;
    }
    const VxCoreError validation_error =
        ValidateRecoveryJournal(notebook, manager, root, entry, journals[i]);
    if (validation_error != VXCORE_OK) {
      return validation_error;
    }
    if (journals[i].value("kind", std::string()) == kKindDestination) {
      const std::string phase = journals[i].value("phase", std::string());
      if (phase == kPhaseCommitted) {
        destination_committed[i] = true;
      } else if (phase == kPhaseDb) {
        if (!journals[i].contains("parentConfigBytes") ||
            !journals[i]["parentConfigBytes"].is_string() ||
            !journals[i].contains("committedParentConfigBytes") ||
            !journals[i]["committedParentConfigBytes"].is_string()) {
          return VXCORE_ERR_INVALID_STATE;
        }
        std::string current_parent_bytes;
        const fs::path parent_config_path = PathFromUtf8(manager->TransferGetConfigPath(
            journals[i].value("destinationParentPath", std::string())));
        if (ReadFile(parent_config_path, current_parent_bytes) != VXCORE_OK) {
          return VXCORE_ERR_IO;
        }
        if (current_parent_bytes == journals[i]["committedParentConfigBytes"].get<std::string>()) {
          destination_committed[i] = true;
        } else if (current_parent_bytes != journals[i]["parentConfigBytes"].get<std::string>()) {
          return VXCORE_ERR_INVALID_STATE;
        }
      }
    }
  }

  int recovered = encryption_recovered;
  for (size_t i = 0; i < entries.size(); ++i) {
    const fs::path &entry = entries[i];
    if (journals[i].is_null()) {
      if (!RemovePathChecked(entry)) {
        return VXCORE_ERR_IO;
      }
      continue;
    }
    const nlohmann::json &journal = journals[i];
    const std::string kind = journal.value("kind", std::string());
    const std::string phase = journal.value("phase", std::string());
    if (kind == kKindDestination) {
      if (!destination_committed[i]) {
        bool staged_exists = false;
        if (!PathExistsChecked(entry / kContentName, staged_exists) ||
            (!staged_exists &&
             !RemovePathChecked(PathFromUtf8(journal.value("contentTarget", std::string()))))) {
          return VXCORE_ERR_IO;
        }
        const std::string metadata = journal.value("metadataTarget", std::string());
        if (!metadata.empty() && (!PathExistsChecked(entry / kMetadataName, staged_exists) ||
                                  (!staged_exists && !RemovePathChecked(PathFromUtf8(metadata))))) {
          return VXCORE_ERR_IO;
        }
        if (journal.contains("assets")) {
          if (!journal["assets"].is_array()) {
            return VXCORE_ERR_INVALID_STATE;
          }
          for (const auto &asset : journal["assets"]) {
            const std::string target = asset.value("target", std::string());
            const fs::path staged = PathFromUtf8(asset.value("staged", std::string()));
            if (target.empty() || staged.empty() || !PathExistsChecked(staged, staged_exists) ||
                (!staged_exists && !RemovePathChecked(PathFromUtf8(target)))) {
              return VXCORE_ERR_IO;
            }
          }
        }
        if (!journal.contains("parentConfigBytes") || !journal["parentConfigBytes"].is_string() ||
            !ReplaceDurable(PathFromUtf8(manager->TransferGetConfigPath(
                                journal.value("destinationParentPath", std::string(".")))),
                            journal["parentConfigBytes"].get<std::string>())) {
          return VXCORE_ERR_IO;
        }
        if (!journal.contains("notebookConfigBytes") ||
            !journal["notebookConfigBytes"].is_string() ||
            !ReplaceDurable(PathFromUtf8(notebook->GetMetadataFolder()) / "config.json",
                            journal["notebookConfigBytes"].get<std::string>())) {
          return VXCORE_ERR_IO;
        }
        const VxCoreError store_error = DeleteJournalStoreIds(notebook, journal, true);
        if (store_error != VXCORE_OK) {
          return store_error;
        }
      }
    } else if (kind == kKindSourceRemoval) {
      if (phase != kPhaseSourceCommitted) {
        const std::string content_quarantine_value =
            journal.value("contentQuarantine", std::string());
        const std::string content_source_value = journal.value("contentSource", std::string());
        if (content_quarantine_value.empty() || content_source_value.empty() ||
            !RestoreRenamedPath(PathFromUtf8(content_quarantine_value),
                                PathFromUtf8(content_source_value))) {
          return VXCORE_ERR_IO;
        }
        const std::string metadata_quarantine = journal.value("metadataQuarantine", std::string());
        if (!metadata_quarantine.empty() &&
            !RestoreRenamedPath(PathFromUtf8(metadata_quarantine),
                                PathFromUtf8(journal.value("metadataSource", std::string())))) {
          return VXCORE_ERR_IO;
        }
        if (journal.contains("assets")) {
          if (!journal["assets"].is_array()) {
            return VXCORE_ERR_INVALID_STATE;
          }
          for (const auto &asset : journal["assets"]) {
            const std::string quarantine = asset.value("quarantine", std::string());
            const std::string source = asset.value("source", std::string());
            if (quarantine.empty() || source.empty() ||
                !RestoreRenamedPath(PathFromUtf8(quarantine), PathFromUtf8(source))) {
              return VXCORE_ERR_IO;
            }
          }
        }
        if (journal.contains("sourceParentConfigBytes") &&
            !ReplaceDurable(PathFromUtf8(manager->TransferGetConfigPath(
                                journal.value("sourceParentPath", std::string(".")))),
                            journal["sourceParentConfigBytes"].get<std::string>())) {
          return VXCORE_ERR_IO;
        }
      } else {
        const VxCoreError store_error = DeleteJournalStoreIds(notebook, journal, false);
        if (store_error != VXCORE_OK) {
          return store_error;
        }
      }
    } else {
      return VXCORE_ERR_INVALID_STATE;
    }
    if (!CleanupJournalDir(entry)) {
      return VXCORE_ERR_IO;
    }
    ++recovered;
  }
  manager->ClearCache();
  if (out_recovered_count) {
    *out_recovered_count = recovered;
  }
  return VXCORE_OK;
}

void NodeTransfer::Discard(std::unique_ptr<PreparedNodeTransfer> transfer) {
  if (transfer) {
    const auto *prepared = static_cast<const PreparedNodeTransferImpl *>(transfer.get());
    RemoveQuietly(PathFromUtf8(prepared->staging_dir));
  }
}


VxCoreError NodeTransfer::CopyWithinNotebook(
    Notebook *notebook, const std::string &source_path, const std::string &destination_folder,
    const std::string &new_name, std::string &out_id, nlohmann::json &out_events) {
  out_id.clear();
  if (!notebook) return VXCORE_ERR_INVALID_PARAM;
  std::unique_ptr<PreparedNodeTransfer> prepared;
  std::string message;
  const nlohmann::json options = {{"operation", "copy"}, {"conflictPolicy", "rename"},
                                  {"timestampPolicy", "reset"}, {"preserveRelativeLinks", true}};
  auto error = Prepare(nullptr, notebook->GetId(), source_path, notebook->GetId(),
                       destination_folder, options, {}, prepared, message, notebook, notebook);
  if (error != VXCORE_OK) return error;
  if (!new_name.empty()) {
    if (!IsSafeRelativePath(new_name, false) || SplitPath(new_name).second != new_name) {
      Discard(std::move(prepared));
      return VXCORE_ERR_INVALID_PARAM;
    }
    auto *snapshot = static_cast<PreparedNodeTransferImpl *>(prepared.get());
    if (!snapshot->is_folder) {
      auto record = snapshot->files.front().destination_record;
      record.name = new_name;
      if (record.CheckProtectionMetadata() == VXCORE_ERR_ENCRYPTION_FORMAT) {
        Discard(std::move(prepared));
        return VXCORE_ERR_ENCRYPTION_FORMAT;
      }
    }
    static_cast<PreparedNodeTransferImpl *>(prepared.get())->source_name = new_name;
  }
  const auto id = static_cast<PreparedNodeTransferImpl *>(prepared.get())->destination_node_id;
  NodeTransferCommitResult result;
  error = Commit(nullptr, std::move(prepared), GenerateUUID(), result, out_events, message);
  if (result.HasCommittedDestination()) out_id = id;
  return error;
}

VxCoreError NodeTransfer::PrepareBundle(
    NotebookManager *manager, const std::string &bundle_root, const std::string &folder_name,
    const std::string &destination_id, const std::string &destination_folder,
    const void *password, size_t password_size, const NodeTransferProgress &progress,
    std::unique_ptr<PreparedNodeTransfer> &out_transfer, std::string &out_error) {
  out_transfer.reset();
  if (!manager || !password || !password_size || !IsSafeRelativePath(folder_name, false) ||
      SplitPath(folder_name).second != folder_name) return VXCORE_ERR_INVALID_PARAM;
  auto *destination = manager->GetNotebook(destination_id);
  if (!destination || !destination->GetEncryption()) return VXCORE_ERR_ENCRYPTION_LOCKED;
  const auto root = PathFromUtf8(bundle_root);
  const auto metadata = root / "vx_notebook";
  if (!root.is_absolute() || HasUnsafeComponent(root, metadata / "config.json") ||
      HasUnsafeComponent(root, metadata / "encryption.vne")) return VXCORE_ERR_INVALID_PARAM;
  std::string bytes;
  auto error = ReadFile(metadata / "config.json", bytes);
  if (error != VXCORE_OK) return error;
  const auto config = NotebookConfig::FromJson(nlohmann::json::parse(bytes));
  if (config.sync_enabled || !IsSafeRelativePath(config.assets_folder, false)) {
    return VXCORE_ERR_ENCRYPTION_FORMAT;
  }
  auto source = std::make_unique<BundleNotebook>(bundle_root, config);
  NotebookEncryption::KeyEnvelope envelope;
  error = NotebookEncryption::ReadKeyEnvelope(metadata / "encryption.vne", envelope);
  if (error != VXCORE_OK) return error;
  NotebookEncryption::Key master, key;
  error = NotebookEncryption::UnlockKeys(envelope, config.id, password, password_size, master, key);
  master.Reset();
  if (error != VXCORE_OK) return error;
  NotebookEncryption *encryption = nullptr;
  error = source->EnsureEncryption(encryption);
  if (error == VXCORE_OK) error = encryption->InstallNotebookKey(envelope, std::move(key));
  if (error != VXCORE_OK) return error;
  const nlohmann::json options = {{"operation", "copy"}, {"conflictPolicy", "rename"},
                                  {"timestampPolicy", "preserve"}, {"preserveRelativeLinks", true}};
  error = Prepare(manager, source->GetId(), folder_name, destination_id, destination_folder,
                  options, progress, out_transfer, out_error, source.get());
  if (error == VXCORE_OK) {
    static_cast<PreparedNodeTransferImpl *>(out_transfer.get())->bundle_source = std::move(source);
  }
  return error;
}
}  // namespace vxcore
