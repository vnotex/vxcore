#include "sync/git/git_config_fixer.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <ios>
#include <sstream>

#include "utils/file_utils.h"
#include "utils/logger.h"

namespace vxcore {

void RebindCoreWorktree(const std::string &root_folder) {
  std::string normalized_workdir = root_folder;
  std::replace(normalized_workdir.begin(), normalized_workdir.end(), '\\', '/');
  if (!normalized_workdir.empty() && normalized_workdir.back() != '/') {
    normalized_workdir.push_back('/');
  }

  const std::string git_config_path = root_folder + "/vx_notebook/vx_sync/config";
  if (PathExists(git_config_path)) {
    try {
      std::ifstream in(PathFromUtf8(git_config_path));
      std::stringstream buf;
      buf << in.rdbuf();
      in.close();
      std::string content = buf.str();

      std::istringstream lines(content);
      std::ostringstream out_lines;
      std::string line;
      bool in_core = false;
      bool rebound = false;
      std::string current_value;
      while (std::getline(lines, line)) {
        std::string trimmed = line;
        size_t start = trimmed.find_first_not_of(" \t");
        if (start != std::string::npos && trimmed[start] == '[') {
          in_core = (trimmed.find("[core]") != std::string::npos);
        } else if (in_core && !rebound) {
          size_t eq = line.find('=');
          if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            size_t key_start = key.find_first_not_of(" \t");
            size_t key_end = key.find_last_not_of(" \t");
            if (key_start != std::string::npos && key_end != std::string::npos &&
                key.substr(key_start, key_end - key_start + 1) == "worktree") {
              std::string val = line.substr(eq + 1);
              size_t val_start = val.find_first_not_of(" \t");
              if (val_start != std::string::npos) {
                current_value = val.substr(val_start);
              }
              line = "\tworktree = " + normalized_workdir;
              rebound = true;
            }
          }
        }
        out_lines << line << '\n';
      }

      if (rebound && current_value != normalized_workdir) {
        std::ofstream out(PathFromUtf8(git_config_path),
                          std::ios::out | std::ios::trunc);
        out << out_lines.str();
        out.close();
        VXCORE_LOG_DEBUG(
            "GitSyncBackend::Initialize: rebound core.worktree from '%s' to '%s' in %s",
            current_value.c_str(), normalized_workdir.c_str(), git_config_path.c_str());
      } else if (rebound) {
        VXCORE_LOG_DEBUG(
            "GitSyncBackend::Initialize: core.worktree already correct (%s), no rebind needed",
            normalized_workdir.c_str());
      } else {
        VXCORE_LOG_DEBUG(
            "GitSyncBackend::Initialize: no [core] worktree found in %s, nothing to rebind",
            git_config_path.c_str());
      }
    } catch (const std::exception &e) {
      VXCORE_LOG_WARN("GitSyncBackend::Initialize: failed to rebind core.worktree in %s: %s",
                      git_config_path.c_str(), e.what());
      // Continue — libgit2 may still succeed or surface a more useful error.
    }
  } else {
    VXCORE_LOG_DEBUG(
        "GitSyncBackend::Initialize: no existing config at %s, first-time bootstrap path",
        git_config_path.c_str());
  }
}

}  // namespace vxcore
