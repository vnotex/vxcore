#include "sync/git/git_repo_bootstrap.h"

#include <filesystem>
#include <system_error>

#include "utils/file_utils.h"
#include "utils/logger.h"

namespace vxcore {

void ScrubGitlink(const std::string &root_folder, const std::string &context) {
  const std::string gitlink_path = root_folder + "/.git";
  if (PathExists(gitlink_path) && !IsDirectory(gitlink_path)) {
    std::error_code rm_ec;
    std::filesystem::remove(PathFromUtf8(gitlink_path), rm_ec);
    if (rm_ec) {
      VXCORE_LOG_WARN("%s: failed to remove gitlink %s: %s",
                      context.c_str(), gitlink_path.c_str(),
                      rm_ec.message().c_str());
    }
  }
}

}  // namespace vxcore
