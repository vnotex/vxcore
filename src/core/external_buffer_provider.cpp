#include "external_buffer_provider.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <vxcore/notebook_json_keys.h>

#include "utils/file_utils.h"
#include "utils/logger.h"

namespace vxcore {

namespace fs = std::filesystem;

namespace {
bool EncryptedSuffix(const std::string &path) noexcept {
  const auto size = path.size();
  return size >= 4 && path[size - 4] == '.' &&
         (path[size - 3] == 'v' || path[size - 3] == 'V') &&
         (path[size - 2] == 'n' || path[size - 2] == 'N') &&
         (path[size - 1] == 'e' || path[size - 1] == 'E');
}
}  // namespace

ExternalBufferProvider::ExternalBufferProvider(const std::string &absolute_file_path) {
  encrypted_ = EncryptedSuffix(absolute_file_path);
  fs::path file_path = PathFromUtf8(CleanPath(absolute_file_path));

  // Extract components
  file_dir_ = PathToUtf8(file_path.parent_path());
  file_name_stem_ = PathToUtf8(file_path.filename().stem());

  // Build assets folder path: <file_dir>/<file_name_stem>_assets
  assets_folder_ = PathToUtf8(PathFromUtf8(file_dir_) / PathFromUtf8(file_name_stem_ + "_assets"));

  VXCORE_LOG_DEBUG("ExternalBufferProvider created for: %s", absolute_file_path.c_str());
  VXCORE_LOG_DEBUG("  file_dir: %s", file_dir_.c_str());
  VXCORE_LOG_DEBUG("  file_name_stem: %s", file_name_stem_.c_str());
  VXCORE_LOG_DEBUG("  assets_folder: %s", assets_folder_.c_str());
}

void ExternalBufferProvider::SetFilePath(const std::string &path) {
  encrypted_ = EncryptedSuffix(path) || marked_encrypted_;
  const auto file_path = PathFromUtf8(CleanPath(path));
  file_dir_ = PathToUtf8(file_path.parent_path());
  file_name_stem_ = PathToUtf8(file_path.filename().stem());
  assets_folder_ = PathToUtf8(PathFromUtf8(file_dir_) / PathFromUtf8(file_name_stem_ + "_assets"));
}

void ExternalBufferProvider::SetFileState(const std::string &path, const nlohmann::json &metadata) {
  const auto marker = metadata.find(kJsonKeyEncrypted);
  marked_encrypted_ = marker != metadata.end() && (!marker->is_boolean() || marker->get<bool>());
  SetFilePath(path);
}

std::string ExternalBufferProvider::GetUniqueAssetName(const std::string &name) {
  fs::path base_path = PathFromUtf8(assets_folder_) / PathFromUtf8(name);

  if (!fs::exists(base_path)) {
    return name;
  }

  // Extract stem and extension
  fs::path name_path = PathFromUtf8(name);
  std::string stem = PathToUtf8(name_path.stem());
  std::string extension = PathToUtf8(name_path.extension());

  // Try suffixes: _1, _2, _3, ...
  for (int i = 1; i < 10000; ++i) {
    std::string candidate = stem + "_" + std::to_string(i) + extension;
    fs::path candidate_path = PathFromUtf8(assets_folder_) / PathFromUtf8(candidate);
    if (!fs::exists(candidate_path)) {
      return candidate;
    }
  }

  // Fallback: append timestamp
  VXCORE_LOG_WARN("Too many collisions for asset name: %s", name.c_str());
  return stem + "_" + std::to_string(std::time(nullptr)) + extension;
}

VxCoreError ExternalBufferProvider::InsertAssetRaw(const std::string &name,
                                                   const std::vector<uint8_t> &data,
                                                   std::string &out_relative_path) {
  if (encrypted_) return VXCORE_ERR_UNSUPPORTED;
  try {
    // Create assets folder if it doesn't exist
    if (!fs::exists(PathFromUtf8(assets_folder_))) {
      fs::create_directories(PathFromUtf8(assets_folder_));
      VXCORE_LOG_INFO("Created assets folder: %s", assets_folder_.c_str());
    }

    // Generate unique name if needed
    std::string unique_name = GetUniqueAssetName(name);

    // Write binary data
    fs::path asset_path = PathFromUtf8(assets_folder_) / PathFromUtf8(unique_name);
    std::ofstream ofs(asset_path, std::ios::binary);
    if (!ofs) {
      VXCORE_LOG_ERROR("Failed to open file for writing: %s", PathToUtf8(asset_path).c_str());
      return VXCORE_ERR_IO;
    }

    ofs.write(reinterpret_cast<const char *>(data.data()), data.size());
    ofs.close();

    if (!ofs) {
      VXCORE_LOG_ERROR("Failed to write file: %s", PathToUtf8(asset_path).c_str());
      return VXCORE_ERR_IO;
    }

    // Build relative path: <file_name_stem>_assets/<unique_name>
    out_relative_path = file_name_stem_ + "_assets/" + unique_name;

    VXCORE_LOG_INFO("Inserted asset (raw): %s", out_relative_path.c_str());
    return VXCORE_OK;

  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("InsertAssetRaw failed: %s", e.what());
    return VXCORE_ERR_IO;
  }
}

VxCoreError ExternalBufferProvider::InsertAsset(const std::string &source_path,
                                                std::string &out_relative_path) {
  if (encrypted_) return VXCORE_ERR_UNSUPPORTED;
  try {
    // Check source file exists
    if (!fs::exists(PathFromUtf8(source_path))) {
      VXCORE_LOG_ERROR("Source file does not exist: %s", source_path.c_str());
      return VXCORE_ERR_NOT_FOUND;
    }

    // Create assets folder if it doesn't exist
    if (!fs::exists(PathFromUtf8(assets_folder_))) {
      fs::create_directories(PathFromUtf8(assets_folder_));
      VXCORE_LOG_INFO("Created assets folder: %s", assets_folder_.c_str());
    }

    // Extract filename from source path
    fs::path src_path = PathFromUtf8(source_path);
    std::string filename = PathToUtf8(src_path.filename());

    // Generate unique name if needed
    std::string unique_name = GetUniqueAssetName(filename);

    // Copy file
    fs::path dest_path = PathFromUtf8(assets_folder_) / PathFromUtf8(unique_name);
    fs::copy_file(PathFromUtf8(source_path), dest_path, fs::copy_options::overwrite_existing);

    // Build relative path: <file_name_stem>_assets/<unique_name>
    out_relative_path = file_name_stem_ + "_assets/" + unique_name;

    VXCORE_LOG_INFO("Inserted asset (copy): %s -> %s", source_path.c_str(),
                    out_relative_path.c_str());
    return VXCORE_OK;

  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("InsertAsset failed: %s", e.what());
    return VXCORE_ERR_IO;
  }
}

VxCoreError ExternalBufferProvider::DeleteAsset(const std::string &relative_path) {
  if (encrypted_) return VXCORE_ERR_UNSUPPORTED;
  try {
    // Convert relative path to absolute: <file_dir>/<relative_path>
    fs::path abs_path = PathFromUtf8(file_dir_) / PathFromUtf8(relative_path);

    if (!fs::exists(abs_path)) {
      VXCORE_LOG_WARN("Asset does not exist: %s", PathToUtf8(abs_path).c_str());
      return VXCORE_ERR_NOT_FOUND;
    }

    fs::remove(abs_path);
    VXCORE_LOG_INFO("Deleted asset: %s", relative_path.c_str());

    return VXCORE_OK;

  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("DeleteAsset failed: %s", e.what());
    return VXCORE_ERR_IO;
  }
}

VxCoreError ExternalBufferProvider::GetAssetsFolder(std::string &out_path) {
  if (encrypted_) { out_path.clear(); return VXCORE_ERR_UNSUPPORTED; }
  try {
    // Create directory if it doesn't exist
    if (!fs::exists(PathFromUtf8(assets_folder_))) {
      fs::create_directories(PathFromUtf8(assets_folder_));
      VXCORE_LOG_INFO("Created assets folder: %s", assets_folder_.c_str());
    }

    out_path = assets_folder_;
    return VXCORE_OK;

  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("GetAssetsFolder failed: %s", e.what());
    return VXCORE_ERR_IO;
  }
}

VxCoreError ExternalBufferProvider::GetAssetAbsolutePath(const std::string &relative_path,
                                                         std::string &out_abs_path) {
  if (encrypted_) { out_abs_path.clear(); return VXCORE_ERR_UNSUPPORTED; }
  // relative_path already includes assets folder name (e.g., "notes_assets/image.png")
  // so just concatenate with file_dir
  out_abs_path = PathToUtf8(PathFromUtf8(file_dir_) / PathFromUtf8(relative_path));
  return VXCORE_OK;
}

VxCoreError ExternalBufferProvider::ReadResource(const std::string &resource_url,
                                                std::vector<uint8_t> &out_data) {
  out_data.clear();
  if (encrypted_) return VXCORE_ERR_UNSUPPORTED;
  try {
    const auto relative = PathFromUtf8(resource_url);
    if (relative.empty() || relative.has_root_path() ||
        resource_url.find('\0') != std::string::npos) {
      return VXCORE_ERR_INVALID_PARAM;
    }
    const auto path = PathFromUtf8(file_dir_) / relative;
    if (!IsPathWithin(file_dir_, PathToUtf8(path), false)) return VXCORE_ERR_INVALID_PARAM;
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

VxCoreError ExternalBufferProvider::GetResourceBasePath(std::string &out_path) {
  if (encrypted_) { out_path.clear(); return VXCORE_ERR_UNSUPPORTED; }
  out_path = CleanPath(file_dir_);
  return VXCORE_OK;
}

// Attachment operations for external files (filesystem only, no metadata)

VxCoreError ExternalBufferProvider::InsertAttachment(const std::string &source_path,
                                                     std::string &out_filename) {
  if (encrypted_) return VXCORE_ERR_UNSUPPORTED;
  // For external files, attachment = asset (filesystem only, no metadata)
  std::string relative_path;
  VxCoreError err = InsertAsset(source_path, relative_path);
  if (err != VXCORE_OK) {
    return err;
  }

  // Extract filename from relative path
  fs::path rel = PathFromUtf8(relative_path);
  out_filename = PathToUtf8(rel.filename());
  VXCORE_LOG_INFO("Inserted attachment (external): %s", out_filename.c_str());
  return VXCORE_OK;
}

VxCoreError ExternalBufferProvider::DeleteAttachment(const std::string &filename) {
  if (encrypted_) return VXCORE_ERR_UNSUPPORTED;
  // Build relative path from filename
  std::string relative_path = file_name_stem_ + "_assets/" + filename;
  VxCoreError err = DeleteAsset(relative_path);
  if (err == VXCORE_OK) {
    VXCORE_LOG_INFO("Deleted attachment (external): %s", filename.c_str());
  }
  return err;
}

VxCoreError ExternalBufferProvider::RenameAttachment(const std::string &old_filename,
                                                     const std::string &new_filename,
                                                     std::string &out_new_filename) {
  if (encrypted_) return VXCORE_ERR_UNSUPPORTED;
  try {
    // Build old absolute path
    fs::path old_abs_path = PathFromUtf8(assets_folder_) / PathFromUtf8(old_filename);

    if (!fs::exists(old_abs_path)) {
      VXCORE_LOG_ERROR("Attachment does not exist: %s", PathToUtf8(old_abs_path).c_str());
      return VXCORE_ERR_NOT_FOUND;
    }

    // Generate unique new name if needed
    std::string unique_new_name = GetUniqueAssetName(new_filename);

    // Build new absolute path
    fs::path new_abs_path = PathFromUtf8(assets_folder_) / PathFromUtf8(unique_new_name);

    // Rename
    fs::rename(old_abs_path, new_abs_path);

    out_new_filename = unique_new_name;

    VXCORE_LOG_INFO("Renamed attachment (external): %s -> %s", old_filename.c_str(),
                    unique_new_name.c_str());

    return VXCORE_OK;

  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("RenameAttachment failed: %s", e.what());
    return VXCORE_ERR_IO;
  }
}

VxCoreError ExternalBufferProvider::ListAttachments(std::vector<std::string> &out_filenames) {
  if (encrypted_) { out_filenames.clear(); return VXCORE_ERR_UNSUPPORTED; }
  out_filenames.clear();

  try {
    // Check if assets folder exists
    if (!fs::exists(PathFromUtf8(assets_folder_))) {
      // No assets folder means no attachments
      return VXCORE_OK;
    }

    if (!fs::is_directory(PathFromUtf8(assets_folder_))) {
      VXCORE_LOG_ERROR("Assets folder is not a directory: %s", assets_folder_.c_str());
      return VXCORE_ERR_IO;
    }

    // Iterate directory entries and return filenames (not full paths)
    for (const auto &entry : fs::directory_iterator(PathFromUtf8(assets_folder_))) {
      if (entry.is_regular_file()) {
        out_filenames.push_back(PathToUtf8(entry.path().filename()));
      }
    }

    VXCORE_LOG_DEBUG("Listed %zu attachments (external)", out_filenames.size());
    return VXCORE_OK;

  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("ListAttachments failed: %s", e.what());
    return VXCORE_ERR_IO;
  }
}

VxCoreError ExternalBufferProvider::GetAttachmentsFolder(std::string &out_path) {
  // Attachments and assets share the same folder
  return GetAssetsFolder(out_path);
}

}  // namespace vxcore
