#include "sync/libgit2_init.h"

#include <git2.h>

#include <atomic>
#include <mutex>

#include "utils/logger.h"

namespace vxcore {

namespace {
static std::atomic<int> g_libgit2_refcount{0};
static std::mutex g_libgit2_init_mutex;
}  // namespace

LibGit2Init::LibGit2Init() {
  std::lock_guard<std::mutex> lock(g_libgit2_init_mutex);
  if (g_libgit2_refcount.fetch_add(1) == 0) {
    int rc = git_libgit2_init();
    if (rc < 0) {
      VXCORE_LOG_ERROR("git_libgit2_init() failed: rc=%d", rc);
    }
  }
}

LibGit2Init::~LibGit2Init() {
  std::lock_guard<std::mutex> lock(g_libgit2_init_mutex);
  if (g_libgit2_refcount.fetch_sub(1) == 1) {
    git_libgit2_shutdown();
  }
}

}  // namespace vxcore
