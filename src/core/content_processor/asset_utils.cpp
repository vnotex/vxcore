#include "core/content_processor/asset_utils.h"

#include <filesystem>

#include "utils/file_utils.h"
#include "utils/logger.h"

namespace fs = std::filesystem;

namespace vxcore {

VxCoreError CopyAssetsDirectory(const std::string &source_assets_dir,
                                const std::string &dest_assets_dir) {
  std::error_code ec;
  if (!fs::exists(source_assets_dir, ec)) {
    return VXCORE_OK;
  }

  fs::path dest_path(dest_assets_dir);
  if (dest_path.has_parent_path()) {
    fs::create_directories(dest_path.parent_path(), ec);
    if (ec) {
      VXCORE_LOG_ERROR("Failed to create parent directories for %s: %s",
                       dest_assets_dir.c_str(), ec.message().c_str());
      return VXCORE_ERR_IO;
    }
  }

  try {
    fs::copy(source_assets_dir, dest_assets_dir,
             fs::copy_options::recursive | fs::copy_options::copy_symlinks);
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to copy assets directory %s -> %s: %s",
                     source_assets_dir.c_str(), dest_assets_dir.c_str(),
                     e.what());
    return VXCORE_ERR_IO;
  }

  return VXCORE_OK;
}

VxCoreError MoveAssetsDirectory(const std::string &source_assets_dir,
                                const std::string &dest_assets_dir) {
  std::error_code ec;
  if (!fs::exists(source_assets_dir, ec)) {
    return VXCORE_OK;
  }

  fs::path dest_path(dest_assets_dir);
  if (dest_path.has_parent_path()) {
    fs::create_directories(dest_path.parent_path(), ec);
    if (ec) {
      VXCORE_LOG_ERROR("Failed to create parent directories for %s: %s",
                       dest_assets_dir.c_str(), ec.message().c_str());
      return VXCORE_ERR_IO;
    }
  }

  // Try rename first (fast, same-volume)
  fs::rename(source_assets_dir, dest_assets_dir, ec);
  if (!ec) {
    return VXCORE_OK;
  }

  // Fall back to copy + remove (cross-volume)
  try {
    fs::copy(source_assets_dir, dest_assets_dir,
             fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    fs::remove_all(source_assets_dir, ec);
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("Failed to move assets directory %s -> %s: %s",
                     source_assets_dir.c_str(), dest_assets_dir.c_str(),
                     e.what());
    return VXCORE_ERR_IO;
  }

  return VXCORE_OK;
}

int CopyRelativeLinkedFiles(const std::vector<std::string> &relative_paths,
                            const std::string &src_dir,
                            const std::string &dest_dir,
                            const std::string &notebook_root,
                            const std::string &source_file_path) {
  int count = 0;
  std::string clean_root = CleanPath(notebook_root);
  std::string clean_source = CleanPath(source_file_path);

  for (const auto &rel_path : relative_paths) {
    try {
      std::string src_abs = CleanPath(ConcatenatePaths(src_dir, rel_path));

      // Skip if outside notebook root.
      if (src_abs.substr(0, clean_root.size()) != clean_root) {
        VXCORE_LOG_WARN("CopyRelativeLinkedFiles: skipping outside notebook: %s",
                        src_abs.c_str());
        continue;
      }

      // Skip self-links.
      if (src_abs == clean_source) {
        continue;
      }

      std::error_code ec;
      if (!fs::is_regular_file(src_abs, ec)) {
        continue;
      }

      std::string dest_abs = CleanPath(ConcatenatePaths(dest_dir, rel_path));
      fs::create_directories(fs::path(dest_abs).parent_path(), ec);
      if (ec) {
        VXCORE_LOG_WARN("CopyRelativeLinkedFiles: failed to create dirs for %s: %s",
                        dest_abs.c_str(), ec.message().c_str());
        continue;
      }

      fs::copy_file(src_abs, dest_abs, fs::copy_options::skip_existing, ec);
      if (ec) {
        VXCORE_LOG_WARN("CopyRelativeLinkedFiles: failed to copy %s: %s",
                        src_abs.c_str(), ec.message().c_str());
        continue;
      }

      ++count;
    } catch (const std::exception &e) {
      VXCORE_LOG_WARN("CopyRelativeLinkedFiles: exception for %s: %s",
                      rel_path.c_str(), e.what());
    }
  }

  return count;
}

int MoveRelativeLinkedFiles(const std::vector<std::string> &relative_paths,
                            const std::string &src_dir,
                            const std::string &dest_dir,
                            const std::string &notebook_root,
                            const std::string &source_file_path) {
  int count = 0;
  std::string clean_root = CleanPath(notebook_root);
  std::string clean_source = CleanPath(source_file_path);

  for (const auto &rel_path : relative_paths) {
    try {
      std::string src_abs = CleanPath(ConcatenatePaths(src_dir, rel_path));

      // Skip if outside notebook root.
      if (src_abs.substr(0, clean_root.size()) != clean_root) {
        VXCORE_LOG_WARN("MoveRelativeLinkedFiles: skipping outside notebook: %s",
                        src_abs.c_str());
        continue;
      }

      // Skip self-links.
      if (src_abs == clean_source) {
        continue;
      }

      std::error_code ec;
      if (!fs::is_regular_file(src_abs, ec)) {
        continue;
      }

      std::string dest_abs = CleanPath(ConcatenatePaths(dest_dir, rel_path));
      fs::create_directories(fs::path(dest_abs).parent_path(), ec);
      if (ec) {
        VXCORE_LOG_WARN("MoveRelativeLinkedFiles: failed to create dirs for %s: %s",
                        dest_abs.c_str(), ec.message().c_str());
        continue;
      }

      // Try rename first (fast, same-volume).
      fs::rename(src_abs, dest_abs, ec);
      if (!ec) {
        ++count;
        continue;
      }

      // Fallback: copy + remove.
      fs::copy_file(src_abs, dest_abs, fs::copy_options::skip_existing, ec);
      if (ec) {
        VXCORE_LOG_WARN("MoveRelativeLinkedFiles: failed to copy %s: %s",
                        src_abs.c_str(), ec.message().c_str());
        continue;
      }

      fs::remove(src_abs, ec);
      ++count;
    } catch (const std::exception &e) {
      VXCORE_LOG_WARN("MoveRelativeLinkedFiles: exception for %s: %s",
                      rel_path.c_str(), e.what());
    }
  }

  return count;
}

}  // namespace vxcore
