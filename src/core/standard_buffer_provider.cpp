// Copyright (c) 2025 VNote
#include "standard_buffer_provider.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <streambuf>
#include <unordered_set>
#include <vxcore/notebook_json_keys.h>

#include "folder.h"
#include "folder_manager.h"
#include "notebook.h"
#include "sync/git/git_conflict_resolver.h"
#include "utils/file_utils.h"
#include "utils/logger.h"

namespace vxcore {

namespace {
using Encryption = NotebookEncryption;
using Json = nlohmann::json;
namespace fs = std::filesystem;

bool EncryptedSuffix(const std::string &path) noexcept {
  const auto size = path.size();
  return size >= 4 && path[size - 4] == '.' &&
         (path[size - 3] == 'v' || path[size - 3] == 'V') &&
         (path[size - 2] == 'n' || path[size - 2] == 'N') &&
         (path[size - 1] == 'e' || path[size - 1] == 'E');
}

template <typename Function>
VxCoreError ProtectedResult(Function &&function) {
  try {
    return function();
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::length_error &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const Json::exception &) {
    return VXCORE_ERR_ENCRYPTION_FORMAT;
  } catch (const fs::filesystem_error &) {
    return VXCORE_ERR_IO;
  } catch (const std::ios_base::failure &) {
    return VXCORE_ERR_IO;
  }
}

struct ManifestCandidate final {
  Json value;
  ~ManifestCandidate() { Encryption::WipeJson(value); }
};

struct BodyCandidate final {
  std::vector<uint8_t> value;
  ~BodyCandidate() { Encryption::WipeBytes(value); }
};

struct NameCandidate final {
  std::string value;
  ~NameCandidate() { Encryption::WipeString(value); }
};

struct AttachmentNames final {
  std::vector<std::string> value;
  ~AttachmentNames() {
    for (auto &name : value) Encryption::WipeString(name);
  }
};

class BorrowedInput final : public std::streambuf {
 public:
  explicit BorrowedInput(const std::vector<uint8_t> &bytes) {
    if (!bytes.empty()) {
      auto *begin = reinterpret_cast<char *>(const_cast<uint8_t *>(bytes.data()));
      setg(begin, begin, begin + bytes.size());
    }
  }
};

const char *MediaTypeForName(const std::string &name) {
  const auto dot = name.rfind('.');
  if (dot == std::string::npos) {
    return "application/octet-stream";
  }
  NameCandidate suffix;
  suffix.value = name.substr(dot);
  auto &extension = suffix.value;
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char byte) {
    return static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte);
  });
  if (extension == ".png") return "image/png";
  if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
  if (extension == ".gif") return "image/gif";
  if (extension == ".webp") return "image/webp";
  if (extension == ".bmp") return "image/bmp";
  if (extension == ".svg") return "image/svg+xml";
  if (extension == ".avif") return "image/avif";
  if (extension == ".txt" || extension == ".md") return "text/plain";
  if (extension == ".pdf") return "application/pdf";
  if (extension == ".json") return "application/json";
  return "application/octet-stream";
}

std::string UniqueAttachmentName(const Json &resources, const std::string &name,
                                  const std::string &excluded_id = {}) {
  auto used = [&](const std::string &candidate) {
    return std::any_of(resources.begin(), resources.end(), [&](const Json &resource) {
      return resource.at(kJsonKeyRole) == "attachment" && resource.at(kJsonKeyName) == candidate &&
             resource.at(kJsonKeyResourceId) != excluded_id;
    });
  };
  if (!used(name)) {
    return name;
  }
  const auto dot = name.rfind('.');
  const auto split = dot == std::string::npos ? name.size() : dot;
  NameCandidate candidate;
  candidate.value.reserve(name.size() + 22);
  for (uint64_t counter = 1;; ++counter) {
    Encryption::WipeString(candidate.value);
    candidate.value.assign(name.data(), split);
    candidate.value += '_';
    candidate.value += std::to_string(counter);
    candidate.value.append(name, split, std::string::npos);
    if (!used(candidate.value)) {
      return std::move(candidate.value);
    }
  }
}

const Json *FindResource(const Json &manifest, const std::string &url) {
  constexpr size_t prefix_size = 8;
  if (url.compare(0, prefix_size, "vxasset:") != 0 ||
      !Encryption::IsCanonicalUuid(url.substr(prefix_size))) {
    return nullptr;
  }
  const auto &resources = manifest.at(kJsonKeyResources);
  for (const auto &resource : resources) {
    const auto &id = resource.at(kJsonKeyResourceId).get_ref<const std::string &>();
    if (url.compare(prefix_size, std::string::npos, id) == 0) {
      return &resource;
    }
  }
  return nullptr;
}
}  // namespace

struct StandardBufferProvider::ProtectedState final {
  // DKs do not implicitly retain their parent. Keep this lease until every note
  // operation, manifest, and committed body owned by this provider is destroyed.
  std::shared_ptr<const Encryption::Key> notebook_key;
  Encryption::KeyEnvelope envelope;
  Encryption::Key note_key;
  Encryption::ObjectHeader header;
  Encryption::Fingerprint fingerprint{};
  Json manifest;
  std::vector<uint8_t> committed_body;
  ~ProtectedState() {
    Encryption::WipeBytes(committed_body);
    Encryption::WipeJson(manifest);
  }
};

StandardBufferProvider::~StandardBufferProvider() = default;

StandardBufferProvider::StandardBufferProvider(Notebook *notebook, const std::string &file_path)
    : notebook_(notebook), file_path_(file_path) {
  if (!notebook_) {
    throw std::invalid_argument("notebook cannot be null");
  }

  // Cache the file ID by retrieving file info
  auto folder_manager = notebook_->GetFolderManager();
  if (!folder_manager) {
    throw std::runtime_error("FolderManager is null");
  }

  const FileRecord *file_record = nullptr;
  VxCoreError err = folder_manager->GetFileInfo(file_path_, &file_record);
  if (err != VXCORE_OK || !file_record) {
    VXCORE_LOG_ERROR("Failed to get file info for %s: error %d", file_path_.c_str(), err);
    throw std::runtime_error("Failed to get file info");
  }

  file_id_ = file_record->id;
  SetFileState(file_path_, file_record->metadata);
}

void StandardBufferProvider::SetFilePath(const std::string &path) {
  file_path_ = path;
  const bool suffix = EncryptedSuffix(path);
  encrypted_ = suffix || metadata_mode_ != 0 || protected_ != nullptr;
  protection_error_ = VXCORE_OK;
  if (encrypted_) {
    if (notebook_->GetType() != NotebookType::Bundled) {
      protection_error_ = VXCORE_ERR_UNSUPPORTED;
    } else if (!suffix || metadata_mode_ == 0 || metadata_mode_ == 3) {
      protection_error_ = VXCORE_ERR_ENCRYPTION_FORMAT;
    } else if (protected_ &&
               protected_->manifest.at(kJsonKeyEditorType) !=
                   (metadata_mode_ == 1 ? "markdown" : "text")) {
      protection_error_ = VXCORE_ERR_ENCRYPTION_FORMAT;
    }
  }
  // Retain the authenticated owner on a malformed refresh. It must never silently
  // downgrade an active protected note, and fixing metadata can restore the handle.
}

void StandardBufferProvider::SetFileState(const std::string &path, const nlohmann::json &metadata) {
  const auto marker = metadata.find(kJsonKeyEncrypted);
  const bool has_marker = marker != metadata.end();
  const bool malformed = has_marker && !marker->is_boolean();
  const bool marked = has_marker && (malformed || marker->get<bool>());
  metadata_mode_ = 0;
  if (marked) {
    metadata_mode_ = 3;
    const auto editor = metadata.find(kJsonKeyEditorType);
    if (!malformed && editor != metadata.end() && editor->is_string()) {
      if (*editor == "markdown") metadata_mode_ = 1;
      else if (*editor == "text") metadata_mode_ = 2;
    }
  }
  SetFilePath(path);
}

VxCoreError StandardBufferProvider::RequireProtectedState(bool writing) const {
  if (!encrypted_) return VXCORE_ERR_UNSUPPORTED;
  if (protection_error_ != VXCORE_OK) return protection_error_;
  const auto conflict_error = GitConflictResolver::CheckEncryptionKeyConflict(
      notebook_->GetMetadataFolder() + "/vx_sync");
  if (conflict_error != VXCORE_OK) return conflict_error;
  if (writing) {
    const auto error = notebook_->CheckWritable();
    if (error != VXCORE_OK) return error;
  }
  if (!protected_) return VXCORE_ERR_ENCRYPTION_LOCKED;
  return VXCORE_OK;
}

VxCoreError StandardBufferProvider::ResolveProtectedPath(const std::filesystem::path &path) const {
  const auto root = PathFromUtf8(notebook_->GetRootFolder()).lexically_normal();
  const auto clean = path.lexically_normal();
  if (!IsPathWithin(notebook_->GetRootFolder(), PathToUtf8(clean), false)) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  const auto relative = clean.lexically_relative(root);
  if (relative.empty() || relative.is_absolute() ||
      CheckReparsePoint(PathToUtf8(root)) != ReparseState::kNo) {
    return VXCORE_ERR_INVALID_PARAM;
  }
  auto current = root;
  for (const auto &part : relative) {
    if (part == "..") return VXCORE_ERR_INVALID_PARAM;
    if (part == ".") continue;
    current /= part;
    std::error_code ec;
    const auto status = fs::symlink_status(current, ec);
    if (ec == std::errc::no_such_file_or_directory) continue;
    if (ec) return VXCORE_ERR_IO;
    if (status.type() != fs::file_type::not_found &&
        CheckReparsePoint(PathToUtf8(current)) != ReparseState::kNo) {
      return VXCORE_ERR_INVALID_PARAM;
    }
  }
  return VXCORE_OK;
}

VxCoreError StandardBufferProvider::ResolveProtectedAssets(std::filesystem::path &out_path,
                                                           bool create) {
  if (!Encryption::IsCanonicalUuid(file_id_)) return VXCORE_ERR_ENCRYPTION_FORMAT;
  const auto assets = GetAssetsFolderPath();
  if (assets.empty()) return VXCORE_ERR_IO;
  const auto path = PathFromUtf8(assets).lexically_normal();
  if (PathToUtf8(path.filename()) != file_id_) return VXCORE_ERR_ENCRYPTION_FORMAT;
  auto error = ResolveProtectedPath(path);
  if (error != VXCORE_OK) return error;
  std::error_code ec;
  if (create) fs::create_directories(path, ec);
  if (ec) return VXCORE_ERR_IO;
  if (fs::exists(path, ec) && !fs::is_directory(path, ec)) return VXCORE_ERR_IO;
  if (ec) return VXCORE_ERR_IO;
  error = ResolveProtectedPath(path);
  if (error == VXCORE_OK) out_path = path;
  return error;
}

VxCoreError StandardBufferProvider::LoadEncryptedContent(std::vector<uint8_t> &out_body) {
  return ProtectedResult([&]() -> VxCoreError {
    if (!encrypted_) return VXCORE_ERR_UNSUPPORTED;
    if (protection_error_ != VXCORE_OK) return protection_error_;
    const auto conflict_error = GitConflictResolver::CheckEncryptionKeyConflict(
        notebook_->GetMetadataFolder() + "/vx_sync");
    if (conflict_error != VXCORE_OK) return conflict_error;
    auto *encryption = notebook_->GetEncryption();
    if (!encryption) return VXCORE_ERR_ENCRYPTION_LOCKED;
    auto candidate = std::make_unique<ProtectedState>();
    auto error = encryption->AcquireNotebookKey(candidate->notebook_key, &candidate->envelope);
    if (error != VXCORE_OK) return error;
    const auto path = PathFromUtf8(notebook_->GetAbsolutePath(file_path_));
    error = ResolveProtectedPath(path);
    if (error != VXCORE_OK) return error;
    error = Encryption::ReadObjectHeader(path, candidate->header);
    if (error != VXCORE_OK) return error;
    if (candidate->header.kind != "note" ||
        (protected_ && candidate->header.document_id != protected_->header.document_id)) {
      return VXCORE_ERR_ENCRYPTION_FORMAT;
    }
    error = Encryption::UnwrapNoteKey(notebook_->GetId(), candidate->envelope.notebook_key_id,
                                     *candidate->notebook_key, candidate->header, candidate->note_key);
    if (error != VXCORE_OK) return error;
    if (protected_ && !Encryption::KeysEqual(protected_->note_key, candidate->note_key)) {
      return VXCORE_ERR_ENCRYPTION_AUTH_FAILED;
    }
    error = Encryption::ReadNoteSnapshot(path, candidate->header, candidate->note_key,
                                         candidate->committed_body, candidate->manifest,
                                         &candidate->fingerprint);
    if (error != VXCORE_OK) return error;
    if (candidate->manifest.at(kJsonKeyEditorType) !=
        (metadata_mode_ == 1 ? "markdown" : "text")) {
      return VXCORE_ERR_ENCRYPTION_FORMAT;
    }
    BodyCandidate body;
    body.value = candidate->committed_body;
    Encryption::WipeBytes(out_body);
    out_body.swap(body.value);
    protected_.swap(candidate);
    return VXCORE_OK;
  });
}

std::string StandardBufferProvider::GetAuthenticatedEditorType() const {
  return protected_ && protection_error_ == VXCORE_OK
             ? protected_->manifest.at(kJsonKeyEditorType).get<std::string>() : std::string();
}

VxCoreError StandardBufferProvider::CheckEncryptedSnapshot(bool &out_matches) {
  out_matches = false;
  return ProtectedResult([&]() -> VxCoreError {
    auto error = RequireProtectedState(false);
    if (error != VXCORE_OK) return error;
    const auto path = PathFromUtf8(notebook_->GetAbsolutePath(file_path_));
    error = ResolveProtectedPath(path);
    if (error != VXCORE_OK) return error;
    Encryption::Fingerprint fingerprint{};
    error = Encryption::FingerprintObject(path, fingerprint);
    if (error == VXCORE_OK) out_matches = fingerprint == protected_->fingerprint;
    return error;
  });
}

VxCoreError StandardBufferProvider::RequireUnchangedSnapshot() {
  bool matches = false;
  const auto error = CheckEncryptedSnapshot(matches);
  return error != VXCORE_OK ? error : (matches ? VXCORE_OK : VXCORE_ERR_FILE_CHANGED_OUTSIDE);
}

VxCoreError StandardBufferProvider::SaveEncryptedContent(const std::vector<uint8_t> &body) {
  return ProtectedResult([&]() -> VxCoreError {
    auto error = RequireProtectedState(true);
    if (error != VXCORE_OK) return error;
    error = RequireUnchangedSnapshot();
    if (error != VXCORE_OK) return error;
    BodyCandidate committed;
    committed.value = body;  // Allocated before publication; only protected notes keep this copy.
    Encryption::ObjectHeader header;
    Encryption::Fingerprint fingerprint{};
    error = Encryption::WriteNoteSnapshot(
        PathFromUtf8(notebook_->GetAbsolutePath(file_path_)), protected_->envelope,
        *protected_->notebook_key, protected_->note_key, protected_->header.document_id,
        body, protected_->manifest, &header, &fingerprint);
    if (error != VXCORE_OK) return error;
    protected_->committed_body.swap(committed.value);
    protected_->header = std::move(header);
    protected_->fingerprint = fingerprint;
    return VXCORE_OK;
  });
}

VxCoreError StandardBufferProvider::WriteEncryptedBackup(const std::vector<uint8_t> &body,
                                                        int revision) {
  return ProtectedResult([&]() -> VxCoreError {
    auto error = RequireProtectedState(true);
    if (error != VXCORE_OK) return error;
    if (revision < 0) return VXCORE_ERR_INVALID_PARAM;
    error = RequireUnchangedSnapshot();
    if (error != VXCORE_OK) return error;
    const auto path = PathFromUtf8(notebook_->GetAbsolutePath(file_path_) + ".vswp");
    error = ResolveProtectedPath(path);
    if (error != VXCORE_OK) return error;
    return Encryption::WriteNoteSnapshot(path, protected_->envelope, *protected_->notebook_key,
                                          protected_->note_key, protected_->header.document_id,
                                          body, protected_->manifest, nullptr, nullptr, revision);
  });
}

VxCoreError StandardBufferProvider::ReadEncryptedBackup(std::vector<uint8_t> &out_body,
                                                       nlohmann::json &out_manifest,
                                                       int &out_revision) {
  const auto path = PathFromUtf8(notebook_->GetAbsolutePath(file_path_) + ".vswp");
  auto error = ResolveProtectedPath(path);
  if (error != VXCORE_OK) return error;
  Encryption::ObjectHeader header;
  error = Encryption::ReadObjectHeader(path, header);
  if (error != VXCORE_OK) return error;
  if (header.kind != "backup" || header.document_id != protected_->header.document_id) {
    return VXCORE_ERR_ENCRYPTION_FORMAT;
  }
  Encryption::Key key;
  error = Encryption::UnwrapNoteKey(notebook_->GetId(), protected_->envelope.notebook_key_id,
                                   *protected_->notebook_key, header, key);
  if (error != VXCORE_OK) return error;
  if (!Encryption::KeysEqual(key, protected_->note_key)) return VXCORE_ERR_ENCRYPTION_AUTH_FAILED;
  error = Encryption::ReadNoteSnapshot(path, header, key, out_body, out_manifest,
                                       nullptr, &out_revision);
  if (error != VXCORE_OK) return error;
  return out_manifest.at(kJsonKeyEditorType) == protected_->manifest.at(kJsonKeyEditorType)
             ? VXCORE_OK : VXCORE_ERR_ENCRYPTION_FORMAT;
}

VxCoreError StandardBufferProvider::RefreshEncryptedBackupManifest(const nlohmann::json &manifest) {
  const auto path = PathFromUtf8(notebook_->GetAbsolutePath(file_path_) + ".vswp");
  auto error = ResolveProtectedPath(path);
  if (error != VXCORE_OK) return error;
  std::error_code ec;
  const bool exists = fs::exists(path, ec);
  if (ec) return VXCORE_ERR_IO;
  if (!exists) return VXCORE_OK;  // None policy must not gain a backup as a side effect.
  if (!fs::is_regular_file(path, ec) || ec) return VXCORE_ERR_IO;
  BodyCandidate body;
  ManifestCandidate previous_manifest;
  int revision = 0;
  error = ReadEncryptedBackup(body.value, previous_manifest.value, revision);
  if (error != VXCORE_OK) return error;
  // Publish a complete dirty snapshot before the live manifest. If the later live
  // write fails, this ahead-of-live backup remains recoverable; this is not a
  // cross-file transaction, and no dirty body/revision is inferred or replaced.
  return Encryption::WriteNoteSnapshot(path, protected_->envelope, *protected_->notebook_key,
                                        protected_->note_key, protected_->header.document_id,
                                        body.value, manifest, nullptr, nullptr, revision);
}

VxCoreError StandardBufferProvider::RecoverEncryptedBackup(std::vector<uint8_t> &out_body,
                                                          int &out_revision) {
  return ProtectedResult([&]() -> VxCoreError {
    auto error = RequireProtectedState(true);
    if (error != VXCORE_OK) return error;
    error = RequireUnchangedSnapshot();
    if (error != VXCORE_OK) return error;
    const auto path = PathFromUtf8(notebook_->GetAbsolutePath(file_path_) + ".vswp");
    BodyCandidate body;
    ManifestCandidate manifest;
    int revision = 0;
    error = ReadEncryptedBackup(body.value, manifest.value, revision);
    if (error != VXCORE_OK) return error;
    BodyCandidate committed;
    committed.value = body.value;
    Encryption::ObjectHeader header;
    Encryption::Fingerprint fingerprint{};
    error = RequireUnchangedSnapshot();
    if (error != VXCORE_OK) return error;
    error = Encryption::WriteNoteSnapshot(
        PathFromUtf8(notebook_->GetAbsolutePath(file_path_)), protected_->envelope,
        *protected_->notebook_key, protected_->note_key, protected_->header.document_id,
        body.value, manifest.value, &header, &fingerprint);
    if (error != VXCORE_OK) return error;
    protected_->committed_body.swap(committed.value);
    protected_->manifest.swap(manifest.value);
    protected_->header = std::move(header);
    protected_->fingerprint = fingerprint;
    Encryption::WipeBytes(out_body);
    out_body.swap(body.value);
    out_revision = revision;
    // The recovered note is now durable. A failed obsolete-backup cleanup cannot
    // turn that successful publication into a reported failed recovery.
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) VXCORE_LOG_WARN("Recovered encrypted note; obsolete backup cleanup failed");
    return VXCORE_OK;
  });
}

VxCoreError StandardBufferProvider::PublishProtectedManifest(nlohmann::json &manifest) {
  auto error = Encryption::ValidateNoteManifest(manifest);
  if (error != VXCORE_OK) return error;
  error = RequireUnchangedSnapshot();
  if (error != VXCORE_OK) return error;
  error = RefreshEncryptedBackupManifest(manifest);
  if (error != VXCORE_OK) return error;
  error = RequireUnchangedSnapshot();
  if (error != VXCORE_OK) return error;
  Encryption::ObjectHeader header;
  Encryption::Fingerprint fingerprint{};
  error = Encryption::WriteNoteSnapshot(
      PathFromUtf8(notebook_->GetAbsolutePath(file_path_)), protected_->envelope,
      *protected_->notebook_key, protected_->note_key, protected_->header.document_id,
      protected_->committed_body, manifest, &header, &fingerprint);
  if (error != VXCORE_OK) return error;
  protected_->manifest.swap(manifest);
  protected_->header = std::move(header);
  protected_->fingerprint = fingerprint;
  notebook_->NotifyEncryptedManifestSaved(file_path_);
  return VXCORE_OK;
}

VxCoreError StandardBufferProvider::InsertProtectedResource(
    const std::string &name, const std::string &media_type, const std::string &role,
    std::istream &data, std::string &out_url) {
  auto error = RequireProtectedState(true);
  if (error != VXCORE_OK) return error;
  error = RequireUnchangedSnapshot();
  if (error != VXCORE_OK) return error;
  ManifestCandidate manifest;
  manifest.value = protected_->manifest;
  auto &resources = manifest.value.at(kJsonKeyResources);
  std::string resource_id;
  std::string object_id;
  error = Encryption::GenerateIdentity(resource_id);
  if (error == VXCORE_OK) error = Encryption::GenerateIdentity(object_id);
  if (error != VXCORE_OK) return error;
  NameCandidate display_name;
  display_name.value = role == "attachment" ? UniqueAttachmentName(resources, name) : name;
  if (role == "comments") {
    for (auto it = resources.begin(); it != resources.end(); ++it) {
      if (it->at(kJsonKeyRole) == "comments") {
        resource_id = it->at(kJsonKeyResourceId).get<std::string>();
        Encryption::WipeJson(*it);
        resources.erase(it);
        break;
      }
    }
  }
  resources.push_back(Json::object());
  auto &resource = resources.back();
  resource[kJsonKeyResourceId] = resource_id;
  resource[kJsonKeyObjectId] = object_id;
  resource[kJsonKeyName] = display_name.value;
  resource[kJsonKeyMediaType] = media_type;
  resource[kJsonKeyRole] = role;
  error = Encryption::ValidateNoteManifest(manifest.value);
  if (error != VXCORE_OK) return error;
  std::string url = "vxasset:" + resource_id;
  fs::path assets;
  error = ResolveProtectedAssets(assets, true);
  if (error != VXCORE_OK) return error;
  const auto destination = assets / PathFromUtf8(object_id + ".vne");
  std::error_code ec;
  if (fs::exists(destination, ec)) return VXCORE_ERR_ALREADY_EXISTS;
  if (ec) return VXCORE_ERR_IO;
  Encryption::ObjectHeader header;
  header.kind = role == "comments" ? "comments" : "asset";
  header.document_id = protected_->header.document_id;
  header.object_id = object_id;
  error = Encryption::EncryptObject(destination, header, protected_->note_key, data);
  if (error != VXCORE_OK) return error;
  // Immutable objects are retained even on failed publication: old saved manifests,
  // backups and conflicts may still select their versions.
  error = PublishProtectedManifest(manifest.value);
  if (error == VXCORE_OK) out_url.swap(url);
  return error;
}

VxCoreError StandardBufferProvider::InsertProtectedFile(const std::string &source_path,
                                                       const std::string &role,
                                                       std::string &out_url) {
  return ProtectedResult([&]() -> VxCoreError {
    auto error = RequireProtectedState(true);
    if (error != VXCORE_OK) return error;
    const auto source = PathFromUtf8(source_path);
    if (source_path.empty() || !source.is_absolute()) return VXCORE_ERR_INVALID_PARAM;
    if (CheckReparsePoint(source_path) != ReparseState::kNo) return VXCORE_ERR_INVALID_PARAM;
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) return ec ? VXCORE_ERR_IO : VXCORE_ERR_NOT_FOUND;
    std::ifstream input;
    input.rdbuf()->pubsetbuf(nullptr, 0);
    input.open(source, std::ios::binary);
    if (!input) return VXCORE_ERR_IO;
    NameCandidate name;
    name.value = PathToUtf8(source.filename());
    return InsertProtectedResource(name.value, MediaTypeForName(name.value), role, input, out_url);
  });
}

VxCoreError StandardBufferProvider::DeleteProtectedResource(const std::string &identity,
                                                           bool attachment) {
  return ProtectedResult([&]() -> VxCoreError {
    auto error = RequireProtectedState(true);
    if (error != VXCORE_OK) return error;
    ManifestCandidate manifest;
    manifest.value = protected_->manifest;
    auto &resources = manifest.value.at(kJsonKeyResources);
    for (auto it = resources.begin(); it != resources.end(); ++it) {
      const auto &role = it->at(kJsonKeyRole);
      const auto &id = it->at(kJsonKeyResourceId).get_ref<const std::string &>();
      const bool matches = identity == "vxasset:" + id;
      if (matches && (attachment ? role == "attachment" : role != "comments")) {
        Encryption::WipeJson(*it);
        resources.erase(it);
        return PublishProtectedManifest(manifest.value);
      }
    }
    return VXCORE_ERR_NOT_FOUND;
  });
}

VxCoreError StandardBufferProvider::ReadProtectedResource(const std::string &url,
                                                         std::vector<uint8_t> &out_data) {
  Encryption::WipeBytes(out_data);
  return ProtectedResult([&]() -> VxCoreError {
    auto error = RequireProtectedState(false);
    if (error != VXCORE_OK) return error;
    const auto *resource = FindResource(protected_->manifest, url);
    if (!resource) return VXCORE_ERR_NOT_FOUND;
    fs::path assets;
    error = ResolveProtectedAssets(assets, false);
    if (error != VXCORE_OK) return error;
    Encryption::ObjectHeader header;
    header.kind = resource->at(kJsonKeyRole) == "comments" ? "comments" : "asset";
    header.document_id = protected_->header.document_id;
    header.object_id = resource->at(kJsonKeyObjectId).get<std::string>();
    const auto path = assets / PathFromUtf8(header.object_id + ".vne");
    error = ResolveProtectedPath(path);
    if (error != VXCORE_OK) return error;
    Encryption::SecureBytes bytes;
    error = Encryption::ReadObject(path, header, protected_->note_key,
                                   std::numeric_limits<size_t>::max(), bytes);
    if (error != VXCORE_OK) return error;
    if (bytes.Size()) out_data.assign(bytes.Data(), bytes.Data() + bytes.Size());
    return VXCORE_OK;
  });
}

VxCoreError StandardBufferProvider::ExportProtectedResource(const std::string &url,
                                                           const std::string &destination_path) {
  return ProtectedResult([&]() -> VxCoreError {
    auto error = RequireProtectedState(false);
    if (error != VXCORE_OK) return error;
    const auto destination = PathFromUtf8(destination_path);
    if (destination_path.empty() || !destination.is_absolute() ||
        IsPathWithin(notebook_->GetRootFolder(), destination_path, true)) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    const auto *resource = FindResource(protected_->manifest, url);
    if (!resource) return VXCORE_ERR_NOT_FOUND;
    fs::path assets;
    error = ResolveProtectedAssets(assets, false);
    if (error != VXCORE_OK) return error;
    Encryption::ObjectHeader header;
    header.kind = resource->at(kJsonKeyRole) == "comments" ? "comments" : "asset";
    header.document_id = protected_->header.document_id;
    header.object_id = resource->at(kJsonKeyObjectId).get<std::string>();
    const auto path = assets / PathFromUtf8(header.object_id + ".vne");
    error = ResolveProtectedPath(path);
    return error == VXCORE_OK ? Encryption::ExportObject(path, header, protected_->note_key, destination)
                             : error;
  });
}

VxCoreError StandardBufferProvider::WriteProtectedComments(const std::vector<uint8_t> &data) {
  return ProtectedResult([&]() -> VxCoreError {
    BorrowedInput buffer(data);
    std::istream input(&buffer);
    std::string url;
    return InsertProtectedResource("comments.json", "application/json", "comments", input, url);
  });
}

VxCoreError StandardBufferProvider::ListProtectedResources(nlohmann::json &out_resources) {
  Encryption::WipeJson(out_resources);
  return ProtectedResult([&]() -> VxCoreError {
    const auto error = RequireProtectedState(false);
    if (error != VXCORE_OK) return error;
    out_resources = protected_->manifest.at(kJsonKeyResources);
    return VXCORE_OK;
  });
}

VxCoreError StandardBufferProvider::InsertAssetRaw(const std::string &name,
                                                   const std::vector<uint8_t> &data,
                                                   std::string &out_relative_path) {
  if (encrypted_) {
    return ProtectedResult([&]() -> VxCoreError {
      BorrowedInput buffer(data);
      std::istream input(&buffer);
      return InsertProtectedResource(name, MediaTypeForName(name), "image", input, out_relative_path);
    });
  }
  if (name.empty()) {
    VXCORE_LOG_ERROR("Asset name cannot be empty");
    return VXCORE_ERR_INVALID_PARAM;
  }

  // Ensure assets folder exists
  VxCoreError err = EnsureAssetsFolderExists();
  if (err != VXCORE_OK) {
    return err;
  }

  // Get the assets folder path
  std::string assets_folder_path = GetAssetsFolderPath();

  // Generate unique name if collision
  std::string unique_name = GetUniqueAssetName(name, assets_folder_path);

  // Construct absolute path for the asset
  std::string asset_abs_path = CleanPath(assets_folder_path + "/" + unique_name);

  // Write binary data to file
  try {
    std::ofstream ofs(PathFromUtf8(asset_abs_path), std::ios::binary);
    if (!ofs) {
      VXCORE_LOG_ERROR("Failed to open file for writing: %s", asset_abs_path.c_str());
      return VXCORE_ERR_IO;
    }

    ofs.write(reinterpret_cast<const char *>(data.data()), data.size());
    if (!ofs) {
      VXCORE_LOG_ERROR("Failed to write data to file: %s", asset_abs_path.c_str());
      return VXCORE_ERR_IO;
    }

    ofs.close();
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Exception while writing asset: %s", e.what());
    return VXCORE_ERR_IO;
  }

  // Compute relative path from notebook root
  std::string notebook_root = notebook_->GetRootFolder();
  std::string relative_path;
  try {
    std::filesystem::path abs_path = PathFromUtf8(asset_abs_path);
    std::filesystem::path root_path = PathFromUtf8(notebook_root);
    relative_path = PathToUtf8(std::filesystem::relative(abs_path, root_path));
    relative_path = CleanPath(relative_path);
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to compute relative path: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }

  // NOTE: Do NOT update attachment metadata here (that's InsertAttachment's job)
  out_relative_path = relative_path;
  return VXCORE_OK;
}

VxCoreError StandardBufferProvider::InsertAsset(const std::string &source_path,
                                                std::string &out_relative_path) {
  if (encrypted_) return InsertProtectedFile(source_path, "image", out_relative_path);
  if (source_path.empty()) {
    VXCORE_LOG_ERROR("Source path cannot be empty");
    return VXCORE_ERR_INVALID_PARAM;
  }

  // Check source file exists
  if (!std::filesystem::exists(PathFromUtf8(source_path))) {
    VXCORE_LOG_ERROR("Source file does not exist: %s", source_path.c_str());
    return VXCORE_ERR_NOT_FOUND;
  }

  // Ensure assets folder exists
  VxCoreError err = EnsureAssetsFolderExists();
  if (err != VXCORE_OK) {
    return err;
  }

  // Get the assets folder path
  std::string assets_folder_path = GetAssetsFolderPath();

  // Extract filename from source path
  std::filesystem::path src_path = PathFromUtf8(source_path);
  std::string filename = PathToUtf8(src_path.filename());

  // Generate unique name if collision
  std::string unique_name = GetUniqueAssetName(filename, assets_folder_path);

  // Construct destination path
  std::string dest_path = CleanPath(assets_folder_path + "/" + unique_name);

  // Copy file
  try {
    std::filesystem::copy_file(PathFromUtf8(source_path), PathFromUtf8(dest_path),
                               std::filesystem::copy_options::overwrite_existing);
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to copy file: %s", e.what());
    return VXCORE_ERR_IO;
  }

  // Compute relative path from notebook root
  std::string notebook_root = notebook_->GetRootFolder();
  std::string relative_path;
  try {
    std::filesystem::path abs_path = PathFromUtf8(dest_path);
    std::filesystem::path root_path = PathFromUtf8(notebook_root);
    relative_path = PathToUtf8(std::filesystem::relative(abs_path, root_path));
    relative_path = CleanPath(relative_path);
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to compute relative path: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }

  out_relative_path = relative_path;
  return VXCORE_OK;
}

VxCoreError StandardBufferProvider::DeleteAsset(const std::string &relative_path) {
  if (encrypted_) return DeleteProtectedResource(relative_path, false);
  if (relative_path.empty()) {
    VXCORE_LOG_ERROR("Relative path cannot be empty");
    return VXCORE_ERR_INVALID_PARAM;
  }

  // Convert relative path to absolute path
  std::string notebook_root = notebook_->GetRootFolder();
  std::string abs_path = CleanPath(notebook_root + "/" + relative_path);

  // Check existence first
  try {
    if (!std::filesystem::exists(PathFromUtf8(abs_path))) {
      VXCORE_LOG_WARN("Asset file does not exist: %s", abs_path.c_str());
      return VXCORE_ERR_NOT_FOUND;
    }
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Exception checking asset existence: %s", e.what());
    return VXCORE_ERR_IO;
  }

  // Prefer recycle bin (bundled notebooks), fallback to permanent delete (raw notebooks).
  auto folder_manager = notebook_->GetFolderManager();
  if (folder_manager) {
    VxCoreError move_err = folder_manager->MoveToRecycleBin(PathFromUtf8(abs_path));
    if (move_err == VXCORE_OK) {
      return VXCORE_OK;
    }
    VXCORE_LOG_WARN("Failed to move asset to recycle bin, fallback to permanent delete: %s",
                    abs_path.c_str());
  }

  // Permanent delete fallback
  try {
    if (!std::filesystem::remove(PathFromUtf8(abs_path))) {
      VXCORE_LOG_ERROR("Failed to delete asset file: %s", abs_path.c_str());
      return VXCORE_ERR_IO;
    }
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Exception while deleting asset: %s", e.what());
    return VXCORE_ERR_IO;
  }

  // NOTE: Do NOT update attachment metadata here (that's DeleteAttachment's job)
  return VXCORE_OK;
}

VxCoreError StandardBufferProvider::InsertAttachment(const std::string &source_path,
                                                     std::string &out_filename) {
  if (encrypted_) return InsertProtectedFile(source_path, "attachment", out_filename);
  if (!AttachmentsSupported()) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  // First, insert the asset (copy file)
  std::string relative_path;
  VxCoreError err = InsertAsset(source_path, relative_path);
  if (err != VXCORE_OK) {
    return err;
  }

  // Track membership by basename; the note already determines the assets directory.
  std::string filename = PathFilename(relative_path);
  auto folder_manager = notebook_->GetFolderManager();
  err = folder_manager->AddFileAttachment(file_path_, filename);
  if (err != VXCORE_OK) {
    VXCORE_LOG_ERROR("Failed to add file attachment: error %d", err);
    // Try to clean up the file we just created
    std::string notebook_root = notebook_->GetRootFolder();
    std::string abs_path = CleanPath(notebook_root + "/" + relative_path);
    try {
      std::filesystem::remove(PathFromUtf8(abs_path));
    } catch (...) {
      // Ignore cleanup errors
    }
    return err;
  }

  out_filename = std::move(filename);
  return VXCORE_OK;
}

VxCoreError StandardBufferProvider::DeleteAttachment(const std::string &filename) {
  if (encrypted_) return DeleteProtectedResource(filename, true);
  if (!AttachmentsSupported()) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  if (!IsSingleName(filename) || filename == "." || filename == ".." ||
      filename.find(':') != std::string::npos || filename.find('\0') != std::string::npos) {
    VXCORE_LOG_ERROR("Attachment must be a basename");
    return VXCORE_ERR_INVALID_PARAM;
  }

  // Build relative path from filename
  std::string assets_folder_path = GetAssetsFolderPath();
  std::string notebook_root = notebook_->GetRootFolder();

  // Compute relative path for the attachment
  std::string abs_path = CleanPath(assets_folder_path + "/" + filename);
  std::string relative_path;
  try {
    std::filesystem::path abs = PathFromUtf8(abs_path);
    std::filesystem::path root = PathFromUtf8(notebook_root);
    relative_path = PathToUtf8(std::filesystem::relative(abs, root));
    relative_path = CleanPath(relative_path);
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to compute relative path: %s", e.what());
    return VXCORE_ERR_UNKNOWN;
  }

  // Delete from metadata first
  auto folder_manager = notebook_->GetFolderManager();
  VxCoreError err = folder_manager->DeleteFileAttachment(file_path_, filename);
  if (err != VXCORE_OK && err != VXCORE_ERR_NOT_FOUND) {
    VXCORE_LOG_WARN("Failed to remove attachment metadata: error %d", err);
  }

  // Delete from filesystem: prefer recycle bin, fallback to permanent delete.
  VxCoreError move_err = folder_manager->MoveToRecycleBin(PathFromUtf8(abs_path));
  if (move_err != VXCORE_OK) {
    VXCORE_LOG_WARN("Failed to move attachment to recycle bin, fallback to permanent delete: %s",
                    abs_path.c_str());
    err = DeleteAsset(relative_path);
  } else {
    err = VXCORE_OK;
  }

  return err;
}

VxCoreError StandardBufferProvider::RenameAttachment(const std::string &old_filename,
                                                     const std::string &new_filename,
                                                     std::string &out_new_filename) {
  if (encrypted_) {
    return ProtectedResult([&]() -> VxCoreError {
      const auto error = RequireProtectedState(true);
      if (error != VXCORE_OK) return error;
      ManifestCandidate manifest;
      manifest.value = protected_->manifest;
      auto &resources = manifest.value.at(kJsonKeyResources);
      for (auto &resource : resources) {
        const auto &id = resource.at(kJsonKeyResourceId).get_ref<const std::string &>();
        if (resource.at(kJsonKeyRole) == "attachment" &&
            old_filename == "vxasset:" + id) {
          NameCandidate name;
          name.value = UniqueAttachmentName(resources, new_filename, id);
          Encryption::WipeJson(resource[kJsonKeyName]);
          resource[kJsonKeyName] = name.value;
          const auto result = PublishProtectedManifest(manifest.value);
          if (result == VXCORE_OK) out_new_filename.swap(name.value);
          return result;
        }
      }
      return VXCORE_ERR_NOT_FOUND;
    });
  }
  if (!AttachmentsSupported()) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  if (!IsSingleName(old_filename) || !IsSingleName(new_filename) || old_filename == "." ||
      old_filename == ".." || new_filename == "." || new_filename == ".." ||
      old_filename.find(':') != std::string::npos || new_filename.find(':') != std::string::npos ||
      old_filename.find('\0') != std::string::npos ||
      new_filename.find('\0') != std::string::npos) {
    VXCORE_LOG_ERROR("Attachment names must be basenames");
    return VXCORE_ERR_INVALID_PARAM;
  }

  std::string assets_folder_path = GetAssetsFolderPath();

  // Build old absolute path
  std::string old_abs_path = CleanPath(assets_folder_path + "/" + old_filename);

  if (!std::filesystem::exists(PathFromUtf8(old_abs_path))) {
    VXCORE_LOG_ERROR("Attachment does not exist: %s", old_abs_path.c_str());
    return VXCORE_ERR_NOT_FOUND;
  }

  // Generate unique name if collision
  std::string unique_name = GetUniqueAssetName(new_filename, assets_folder_path);

  // Build new absolute path
  std::string new_abs_path = CleanPath(assets_folder_path + "/" + unique_name);

  try {
    // Rename the file
    std::filesystem::rename(PathFromUtf8(old_abs_path), PathFromUtf8(new_abs_path));
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to rename attachment: %s", e.what());
    return VXCORE_ERR_IO;
  }

  // Update metadata: remove old attachment, add new
  auto folder_manager = notebook_->GetFolderManager();
  VxCoreError err = folder_manager->DeleteFileAttachment(file_path_, old_filename);
  if (err != VXCORE_OK && err != VXCORE_ERR_NOT_FOUND) {
    VXCORE_LOG_WARN("Failed to remove old attachment metadata: error %d", err);
  }

  err = folder_manager->AddFileAttachment(file_path_, unique_name);
  if (err != VXCORE_OK) {
    VXCORE_LOG_WARN("Failed to add new attachment metadata: error %d", err);
  }

  out_new_filename = unique_name;
  VXCORE_LOG_INFO("Renamed attachment: %s -> %s", old_filename.c_str(), unique_name.c_str());
  return VXCORE_OK;
}

VxCoreError StandardBufferProvider::ListAttachments(std::vector<std::string> &out_filenames) {
  if (encrypted_) {
    for (auto &name : out_filenames) Encryption::WipeString(name);
    out_filenames.clear();
    return ProtectedResult([&]() -> VxCoreError {
      const auto error = RequireProtectedState(false);
      if (error != VXCORE_OK) return error;
      AttachmentNames names;
      const auto &resources = protected_->manifest.at(kJsonKeyResources);
      const auto count = std::count_if(resources.begin(), resources.end(), [](const Json &resource) {
        return resource.at(kJsonKeyRole) == "attachment";
      });
      names.value.reserve(static_cast<size_t>(count));
      for (const auto &resource : resources) {
        if (resource.at(kJsonKeyRole) == "attachment") {
          names.value.emplace_back(resource.at(kJsonKeyName).get_ref<const std::string &>());
        }
      }
      out_filenames.swap(names.value);
      return VXCORE_OK;
    });
  }
  if (!AttachmentsSupported()) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  out_filenames.clear();

  auto folder_manager = notebook_->GetFolderManager();
  if (!folder_manager) {
    VXCORE_LOG_ERROR("FolderManager is null");
    return VXCORE_ERR_UNKNOWN;
  }

  std::string attachments_json;
  VxCoreError err = folder_manager->GetFileAttachments(file_path_, attachments_json);
  if (err != VXCORE_OK) {
    return err;
  }

  try {
    nlohmann::json j = nlohmann::json::parse(attachments_json);
    if (j.is_array()) {
      // Keep malformed legacy metadata from exposing paths through the public filename list.
      for (const auto &attachment : j) {
        out_filenames.push_back(PathFilename(attachment.get<std::string>()));
      }
    }
    return VXCORE_OK;
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to parse attachments JSON: %s", e.what());
    return VXCORE_ERR_JSON_PARSE;
  }
}

VxCoreError StandardBufferProvider::ListUnindexedAttachments(
    std::vector<std::string> &out_filenames) {
  out_filenames.clear();
  std::vector<std::string> attachments;
  const auto err = ListAttachments(attachments);
  if (err != VXCORE_OK) {
    return err;
  }

  const auto folder = GetAssetsFolderPath();
  if (folder.empty()) {
    return VXCORE_ERR_IO;
  }
  std::error_code ec;
  const auto path = PathFromUtf8(folder);
  const auto status = std::filesystem::status(path, ec);
  if (status.type() == std::filesystem::file_type::not_found &&
      (!ec || ec == std::errc::no_such_file_or_directory)) {
    return VXCORE_OK;
  }
  if (ec || !std::filesystem::is_directory(status)) {
    return VXCORE_ERR_IO;
  }

  const std::unordered_set<std::string> indexed(attachments.begin(), attachments.end());
  std::vector<std::string> filenames;
  std::filesystem::directory_iterator it(path, ec), end;
  while (!ec && it != end) {
    const auto entry_status = it->symlink_status(ec);
    if (ec) {
      break;
    }
    if (std::filesystem::is_regular_file(entry_status)) {
      auto filename = PathToUtf8(it->path().filename());
      auto normalized = filename;
      if (FileRecord::NormalizeAttachmentName(normalized) && normalized == filename &&
          indexed.find(filename) == indexed.end()) {
        filenames.push_back(std::move(filename));
      }
    }
    it.increment(ec);
  }
  if (ec) {
    return VXCORE_ERR_IO;
  }
  std::sort(filenames.begin(), filenames.end());
  out_filenames = std::move(filenames);
  return VXCORE_OK;
}

std::string StandardBufferProvider::GetAssetsFolderPath() {
  auto folder_manager = notebook_->GetFolderManager();
  if (!folder_manager) {
    VXCORE_LOG_ERROR("FolderManager is null");
    return "";
  }

  std::string assets_folder = folder_manager->GetAssetsFolder(file_path_);
  return assets_folder;
}

bool StandardBufferProvider::AttachmentsSupported() const {
  return notebook_ && notebook_->GetTypeStr() == "bundled";
}

VxCoreError StandardBufferProvider::GetAssetsFolder(std::string &out_path) {
  if (encrypted_) {
    out_path.clear();
    return protection_error_ == VXCORE_OK ? VXCORE_ERR_UNSUPPORTED : protection_error_;
  }
  std::string path = GetAssetsFolderPath();
  if (path.empty()) {
    return VXCORE_ERR_UNKNOWN;
  }

  // Create folder lazily if it doesn't exist
  VxCoreError err = EnsureAssetsFolderExists();
  if (err != VXCORE_OK) {
    return err;
  }

  out_path = path;
  return VXCORE_OK;
}

VxCoreError StandardBufferProvider::GetAttachmentsFolder(std::string &out_path) {
  if (!AttachmentsSupported()) {
    return VXCORE_ERR_UNSUPPORTED;
  }

  // Attachments and assets share the same folder
  return GetAssetsFolder(out_path);
}

VxCoreError StandardBufferProvider::GetAssetAbsolutePath(const std::string &relative_path,
                                                         std::string &out_abs_path) {
  if (encrypted_) {
    out_abs_path.clear();
    return protection_error_ == VXCORE_OK ? VXCORE_ERR_UNSUPPORTED : protection_error_;
  }
  if (relative_path.empty()) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  std::string notebook_root = notebook_->GetRootFolder();
  out_abs_path = CleanPath(notebook_root + "/" + relative_path);
  return VXCORE_OK;
}

VxCoreError StandardBufferProvider::ReadResource(const std::string &resource_url,
                                                std::vector<uint8_t> &out_data) {
  if (encrypted_) return ReadProtectedResource(resource_url, out_data);
  out_data.clear();
  try {
    const auto relative = PathFromUtf8(resource_url);
    if (relative.empty() || relative.has_root_path() ||
        resource_url.find('\0') != std::string::npos) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    const auto &root = notebook_->GetRootFolder();
    const auto path = PathFromUtf8(root) / relative;
    if (!IsPathWithin(root, PathToUtf8(path), false)) return VXCORE_ERR_INVALID_PARAM;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return ec ? VXCORE_ERR_IO : VXCORE_ERR_NOT_FOUND;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return VXCORE_ERR_IO;
    const std::streamoff size = input.tellg();
    if (size < 0) return VXCORE_ERR_IO;
    if (static_cast<uintmax_t>(size) > out_data.max_size() ||
        size > std::numeric_limits<std::streamsize>::max()) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }
    out_data.resize(static_cast<size_t>(size));
    input.seekg(0);
    if (!input || (size && !input.read(reinterpret_cast<char *>(out_data.data()),
                                       static_cast<std::streamsize>(size)))) {
      out_data.clear();
      return VXCORE_ERR_IO;
    }
    return VXCORE_OK;
  } catch (const std::bad_alloc &) {
    out_data.clear();
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::length_error &) {
    out_data.clear();
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (...) {
    out_data.clear();
    return VXCORE_ERR_IO;
  }
}

VxCoreError StandardBufferProvider::GetResourceBasePath(std::string &out_path) {
  if (encrypted_) {
    out_path.clear();
    return protection_error_ == VXCORE_OK ? VXCORE_ERR_UNSUPPORTED : protection_error_;
  }
  std::string abs_path = CleanPath(notebook_->GetRootFolder() + "/" + file_path_);
  std::filesystem::path p = PathFromUtf8(abs_path);
  out_path = CleanFsPath(p.parent_path());
  return VXCORE_OK;
}

VxCoreError StandardBufferProvider::EnsureAssetsFolderExists() {
  std::string assets_folder = GetAssetsFolderPath();
  if (assets_folder.empty()) {
    VXCORE_LOG_ERROR("Failed to get assets folder path");
    return VXCORE_ERR_UNKNOWN;
  }

  try {
    if (!std::filesystem::exists(PathFromUtf8(assets_folder))) {
      if (!std::filesystem::create_directories(PathFromUtf8(assets_folder))) {
        VXCORE_LOG_ERROR("Failed to create assets folder: %s", assets_folder.c_str());
        return VXCORE_ERR_IO;
      }
    }
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Exception while creating assets folder: %s", e.what());
    return VXCORE_ERR_IO;
  }

  return VXCORE_OK;
}

std::string StandardBufferProvider::GetUniqueAssetName(const std::string &base_name,
                                                       const std::string &assets_folder_path) {
  std::string candidate = base_name;
  std::string full_path = CleanPath(assets_folder_path + "/" + candidate);

  int counter = 1;
  while (std::filesystem::exists(PathFromUtf8(full_path))) {
    // Extract extension
    size_t dot_pos = base_name.find_last_of('.');
    std::string name_part;
    std::string ext_part;

    if (dot_pos != std::string::npos) {
      name_part = base_name.substr(0, dot_pos);
      ext_part = base_name.substr(dot_pos);
    } else {
      name_part = base_name;
      ext_part = "";
    }

    candidate = name_part + "_" + std::to_string(counter) + ext_part;
    full_path = CleanPath(assets_folder_path + "/" + candidate);
    counter++;
  }

  return candidate;
}

}  // namespace vxcore
