#include "sync/git/git_defaults.h"

#include <exception>
#include <fstream>
#include <ios>

#include "utils/file_utils.h"
#include "utils/logger.h"

namespace vxcore {

const char *const kDefaultGitignore =
    "# VNote sync defaults — feel free to edit; this file is preserved on update.\n"
    "vx_notebook/vx_sync/\n"
    "*.vswp\n"
    "*.tmp\n"
    ".DS_Store\n"
    "Thumbs.db\n";

const char *const kDefaultGitattributes =
    "# VNote sync defaults — feel free to edit; this file is preserved on update.\n"
    "* text=auto eol=lf\n"
    "*.png binary\n"
    "*.jpg binary\n"
    "*.jpeg binary\n"
    "*.gif binary\n"
    "*.pdf binary\n"
    "*.zip binary\n"
    "*.mp4 binary\n";

const char *const kDefaultAuthorName = "VNote Sync";

const char *const kDefaultAuthorEmail = "sync@vnote.local";

bool WriteIfMissing(const std::string &abs_path, const std::string &content) {
  if (PathExists(abs_path)) {
    return false;
  }
  try {
    std::ofstream ofs(PathFromUtf8(abs_path), std::ios::binary | std::ios::trunc);
    if (!ofs) {
      VXCORE_LOG_ERROR("WriteIfMissing: failed to open %s for writing", abs_path.c_str());
      return false;
    }
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!ofs) {
      VXCORE_LOG_ERROR("WriteIfMissing: failed to write %s", abs_path.c_str());
      return false;
    }
    return true;
  } catch (const std::exception &e) {
    VXCORE_LOG_ERROR("WriteIfMissing(%s) exception: %s", abs_path.c_str(), e.what());
    return false;
  }
}

std::string BuildGitignoreContent(const SyncConfig &config) {
  std::string out(kDefaultGitignore);
  if (!config.exclude_paths.empty()) {
    out += "\n# From SyncConfig::exclude_paths\n";
    for (const auto &p : config.exclude_paths) {
      out += p;
      out += '\n';
    }
  }
  return out;
}

}  // namespace vxcore
