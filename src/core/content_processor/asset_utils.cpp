#include "core/content_processor/asset_utils.h"

#include <filesystem>

#include "utils/file_utils.h"
#include "utils/logger.h"

namespace fs = std::filesystem;

namespace vxcore {

VxCoreError CopyAssetsDirectory(const std::string &source_assets_dir,
                                const std::string &dest_assets_dir) {
  // NOTE: every std::filesystem boundary below MUST go through PathFromUtf8().
  // Passing a UTF-8 std::string straight to std::filesystem makes MSVC convert
  // it with the active ANSI code page, which THROWS std::system_error
  // (ERROR_NO_UNICODE_TRANSLATION) when the path holds characters the ACP
  // cannot encode. The whole body is wrapped so no exception can escape.
  try {
    const fs::path source_path = PathFromUtf8(source_assets_dir);
    const fs::path dest_path = PathFromUtf8(dest_assets_dir);

    std::error_code ec;
    // Distinguish "no assets to copy" (fine) from "cannot tell" (an I/O fault
    // the caller must hear about) — returning OK for the latter would make a
    // failed copy look like a note that simply had no assets.
    const bool source_exists = fs::exists(source_path, ec);
    if (ec) {
      VXCORE_LOG_ERROR("Failed to stat assets directory %s: %s", source_assets_dir.c_str(),
                       ec.message().c_str());
      return VXCORE_ERR_IO;
    }
    if (!source_exists) {
      return VXCORE_OK;
    }

    if (dest_path.has_parent_path()) {
      fs::create_directories(dest_path.parent_path(), ec);
      if (ec) {
        VXCORE_LOG_ERROR("Failed to create parent directories for %s: %s",
                         dest_assets_dir.c_str(), ec.message().c_str());
        return VXCORE_ERR_IO;
      }
    }

    fs::copy(source_path, dest_path,
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
  // NOTE: see CopyAssetsDirectory above -- PathFromUtf8() at every
  // std::filesystem boundary, and the whole body is wrapped so no exception
  // can escape into the caller's move/copy transaction.
  try {
    const fs::path source_path = PathFromUtf8(source_assets_dir);
    const fs::path dest_path = PathFromUtf8(dest_assets_dir);

    std::error_code ec;
    // See CopyAssetsDirectory: an unreadable source is an I/O fault, not
    // "there were no assets".
    const bool source_exists = fs::exists(source_path, ec);
    if (ec) {
      VXCORE_LOG_ERROR("Failed to stat assets directory %s: %s", source_assets_dir.c_str(),
                       ec.message().c_str());
      return VXCORE_ERR_IO;
    }
    if (!source_exists) {
      return VXCORE_OK;
    }

    if (dest_path.has_parent_path()) {
      fs::create_directories(dest_path.parent_path(), ec);
      if (ec) {
        VXCORE_LOG_ERROR("Failed to create parent directories for %s: %s",
                         dest_assets_dir.c_str(), ec.message().c_str());
        return VXCORE_ERR_IO;
      }
    }

    // Try rename first (fast, same-volume)
    fs::rename(source_path, dest_path, ec);
    if (!ec) {
      return VXCORE_OK;
    }

    // Fall back to copy + remove (cross-volume)
    fs::copy(source_path, dest_path,
             fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    fs::remove_all(source_path, ec);
    if (ec) {
      // The destination copy exists, so the assets are not lost, but the source
      // is still there — report it rather than claiming a clean move.
      VXCORE_LOG_ERROR("Moved assets to %s but failed to remove source %s: %s",
                       dest_assets_dir.c_str(), source_assets_dir.c_str(), ec.message().c_str());
      return VXCORE_ERR_IO;
    }
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
      const fs::path src_fs_path = PathFromUtf8(src_abs);
      if (!fs::is_regular_file(src_fs_path, ec)) {
        continue;
      }

      std::string dest_abs = CleanPath(ConcatenatePaths(dest_dir, rel_path));
      const fs::path dest_fs_path = PathFromUtf8(dest_abs);
      fs::create_directories(dest_fs_path.parent_path(), ec);
      if (ec) {
        VXCORE_LOG_WARN("CopyRelativeLinkedFiles: failed to create dirs for %s: %s",
                        dest_abs.c_str(), ec.message().c_str());
        continue;
      }

      fs::copy_file(src_fs_path, dest_fs_path, fs::copy_options::skip_existing, ec);
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
      const fs::path src_fs_path = PathFromUtf8(src_abs);
      if (!fs::is_regular_file(src_fs_path, ec)) {
        continue;
      }

      std::string dest_abs = CleanPath(ConcatenatePaths(dest_dir, rel_path));
      const fs::path dest_fs_path = PathFromUtf8(dest_abs);
      fs::create_directories(dest_fs_path.parent_path(), ec);
      if (ec) {
        VXCORE_LOG_WARN("MoveRelativeLinkedFiles: failed to create dirs for %s: %s",
                        dest_abs.c_str(), ec.message().c_str());
        continue;
      }

      // Try rename first (fast, same-volume).
      fs::rename(src_fs_path, dest_fs_path, ec);
      if (!ec) {
        ++count;
        continue;
      }

      // Fallback: copy + remove.
      fs::copy_file(src_fs_path, dest_fs_path, fs::copy_options::skip_existing, ec);
      if (ec) {
        VXCORE_LOG_WARN("MoveRelativeLinkedFiles: failed to copy %s: %s",
                        src_abs.c_str(), ec.message().c_str());
        continue;
      }

      fs::remove(src_fs_path, ec);
      ++count;
    } catch (const std::exception &e) {
      VXCORE_LOG_WARN("MoveRelativeLinkedFiles: exception for %s: %s",
                      rel_path.c_str(), e.what());
    }
  }

  return count;
}

}  // namespace vxcore
