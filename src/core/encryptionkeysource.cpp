#include "encryptionkeysource.h"

#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

#include "sync/sync_encryption_guard.h"
#include "utils/file_utils.h"

namespace vxcore {
namespace {

namespace fs = std::filesystem;
using Encryption = EncryptedNote;

template <typename Function>
VxCoreError SourceResult(Function &&function) {
  try {
    return function();
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::length_error &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const fs::filesystem_error &) {
    return VXCORE_ERR_IO;
  }
}

VxCoreError SafeSourcePath(const fs::path &path, const fs::path &root,
                           VxCoreError missing_error) {
  if (path.empty() || root.empty()) return VXCORE_ERR_INVALID_PARAM;
  for (const auto &part : path) {
    if (part == "..") return VXCORE_ERR_INVALID_PARAM;
  }
  std::error_code ec;
  const auto absolute = fs::absolute(path, ec).lexically_normal();
  if (ec) return VXCORE_ERR_IO;
  const auto trusted_root = fs::absolute(root, ec).lexically_normal();
  if (ec) return VXCORE_ERR_IO;
  const auto relative = absolute.lexically_relative(trusted_root);
  if (relative.empty() || relative.is_absolute()) return VXCORE_ERR_INVALID_PARAM;
  for (const auto &part : relative) {
    if (part == "..") return VXCORE_ERR_INVALID_PARAM;
  }
  // Ancestors above the notebook may be aliases. The root and every descendant
  // must be real entries; together with lexical containment this rejects escapes.
  auto current = trusted_root;
  const auto root_status = fs::symlink_status(current, ec);
  if (ec == std::errc::no_such_file_or_directory ||
      (!ec && root_status.type() == fs::file_type::not_found)) {
    return missing_error;
  }
  if (ec) return VXCORE_ERR_IO;
  const auto root_reparse = CheckReparsePoint(PathToUtf8(current));
  if (root_reparse != ReparseState::kNo) {
    return root_reparse == ReparseState::kError ? VXCORE_ERR_IO : VXCORE_ERR_INVALID_PARAM;
  }
  if (!fs::is_directory(root_status)) return VXCORE_ERR_INVALID_PARAM;
  if (relative == ".") return VXCORE_ERR_ENCRYPTION_FORMAT;
  for (const auto &part : relative) {
    if (part == ".") continue;
    current /= part;
    const auto status = fs::symlink_status(current, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == fs::file_type::not_found)) {
      return missing_error;
    }
    if (ec) return VXCORE_ERR_IO;
    const auto reparse = CheckReparsePoint(PathToUtf8(current));
    if (reparse != ReparseState::kNo) {
      return reparse == ReparseState::kError ? VXCORE_ERR_IO : VXCORE_ERR_INVALID_PARAM;
    }
    if (current == absolute) {
      if (!fs::is_regular_file(status)) return VXCORE_ERR_ENCRYPTION_FORMAT;
    } else if (!fs::is_directory(status)) {
      return VXCORE_ERR_INVALID_PARAM;
    }
  }
  return VXCORE_OK;
}

VxCoreError ReadLegacyEnvelope(const fs::path &metadata_folder, const fs::path &root,
                               Encryption::KeyEnvelope &out_envelope) {
  const auto path = metadata_folder / "encryption.vne";
  auto error = SafeSourcePath(path, root, VXCORE_ERR_ENCRYPTION_RECOVERY_REQUIRED);
  if (error != VXCORE_OK) return error;
  error = CheckNotebookEncryptionSyncState(PathToUtf8(metadata_folder));
  return error == VXCORE_OK ? Encryption::ReadKeyEnvelope(path, out_envelope) : error;
}

class SelfContainedNoteKeySource final : public EncryptionKeySource {
 public:
  SelfContainedNoteKeySource(fs::path path, fs::path root, Encryption::ObjectHeader header)
      : path_(std::move(path)), root_(std::move(root)), header_(std::move(header)) {}

  const Encryption::KeyEnvelope &Envelope() const noexcept override { return header_.envelope; }
  const Encryption::ObjectHeader *Header() const noexcept override { return &header_; }
  bool IsLegacy() const noexcept override { return false; }
  VxCoreError Revalidate() const override {
    return SourceResult([&]() -> VxCoreError {
      auto error = SafeSourcePath(path_, root_, VXCORE_ERR_NODE_NOT_EXISTS);
      if (error != VXCORE_OK) return error;
      Encryption::ObjectHeader current;
      error = Encryption::ReadObjectHeader(path_, current);
      if (error != VXCORE_OK) return error;
      return Encryption::SameObject(header_, current) ? VXCORE_OK : VXCORE_ERR_FILE_CHANGED_OUTSIDE;
    });
  }

 private:
  fs::path path_;
  fs::path root_;
  Encryption::ObjectHeader header_;
};

class NotebookVaultKeySource final : public EncryptionKeySource {
 public:
  NotebookVaultKeySource(fs::path path, fs::path metadata_folder, fs::path root,
                        Encryption::KeyEnvelope envelope,
                        std::optional<Encryption::ObjectHeader> header)
      : path_(std::move(path)), metadata_folder_(std::move(metadata_folder)),
        root_(std::move(root)), envelope_(std::move(envelope)), header_(std::move(header)) {}

  const Encryption::KeyEnvelope &Envelope() const noexcept override { return envelope_; }
  const Encryption::ObjectHeader *Header() const noexcept override {
    return header_ ? &*header_ : nullptr;
  }
  bool IsLegacy() const noexcept override { return true; }
  VxCoreError Revalidate() const override {
    return SourceResult([&]() -> VxCoreError {
      if (header_) {
        auto error = SafeSourcePath(path_, root_, VXCORE_ERR_NODE_NOT_EXISTS);
        if (error != VXCORE_OK) return error;
        Encryption::ObjectHeader current;
        error = Encryption::ReadObjectHeader(path_, current);
        if (error != VXCORE_OK) return error;
        if (!Encryption::SameObject(*header_, current)) return VXCORE_ERR_FILE_CHANGED_OUTSIDE;
      }
      Encryption::KeyEnvelope current;
      const auto error = ReadLegacyEnvelope(metadata_folder_, root_, current);
      if (error != VXCORE_OK) return error;
      return Encryption::SameEnvelope(envelope_, current) ? VXCORE_OK : VXCORE_ERR_FILE_CHANGED_OUTSIDE;
    });
  }

 private:
  fs::path path_;
  fs::path metadata_folder_;
  fs::path root_;
  Encryption::KeyEnvelope envelope_;
  std::optional<Encryption::ObjectHeader> header_;
};

}  // namespace

VxCoreError EncryptionKeySource::Read(const fs::path &note_path, const fs::path &metadata_folder,
                                      std::unique_ptr<EncryptionKeySource> &out_source) {
  return SourceResult([&]() -> VxCoreError {
    if (metadata_folder.empty()) return VXCORE_ERR_INVALID_PARAM;
    std::error_code ec;
    const auto metadata = fs::absolute(metadata_folder, ec).lexically_normal();
    if (ec) return VXCORE_ERR_IO;
    const auto root = metadata.parent_path();
    std::optional<Encryption::ObjectHeader> header;
    fs::path selected;
    if (!note_path.empty()) {
      auto error = SafeSourcePath(note_path, root, VXCORE_ERR_NODE_NOT_EXISTS);
      if (error != VXCORE_OK) return error;
      selected = fs::absolute(note_path, ec).lexically_normal();
      if (ec) return VXCORE_ERR_IO;
      header.emplace();
      error = Encryption::ReadObjectHeader(selected, *header);
      if (error != VXCORE_OK) return error;
      if (header->version == 2) {
        out_source = std::make_unique<SelfContainedNoteKeySource>(selected, root,
                                                                 std::move(*header));
        return VXCORE_OK;
      }
    }
    Encryption::KeyEnvelope envelope;
    auto error = ReadLegacyEnvelope(metadata, root, envelope);
    if (error != VXCORE_OK) return error;
    if (header && header->notebook_key_id != envelope.notebook_key_id) {
      return VXCORE_ERR_ENCRYPTION_FORMAT;
    }
    out_source = std::make_unique<NotebookVaultKeySource>(selected, metadata, root,
                                                         std::move(envelope), std::move(header));
    return VXCORE_OK;
  });
}

}  // namespace vxcore
