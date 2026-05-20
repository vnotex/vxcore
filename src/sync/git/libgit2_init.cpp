#include "sync/git/libgit2_init.h"

#include <git2.h>

#include <atomic>
#include <mutex>

#include "utils/logger.h"

namespace vxcore {

namespace {
static std::atomic<int> g_libgit2_refcount{0};
static std::atomic<bool> g_libgit2_initialized{false};
static std::mutex g_libgit2_init_mutex;
}  // namespace

// Cross-TU anchor hook (Task 4.3 of sync-backend-phase4): git_sync_backend.cpp
// stores a non-null pointer to its EnsureGitBackendLinked function here via
// a static initializer. LibGit2Init::ctor invokes the pointer once on first
// use, which references a symbol in git_sync_backend.cpp and thus prevents
// MSVC /OPT:REF from dead-stripping that TU (and with it, the static
// BackendRegistration token that self-registers the git backend).
//
// When git_sync_backend.cpp is NOT linked (e.g. test_libgit2_init,
// test_libgit2_init_propagation, test_sync_manager_unknown_backend), the
// pointer simply stays null and the call is a no-op — no extra test churn
// required.
std::atomic<void (*)()> g_git_backend_link_anchor{nullptr};

LibGit2Init::LibGit2Init() {
  std::lock_guard<std::mutex> lock(g_libgit2_init_mutex);
  if (g_libgit2_refcount.fetch_add(1) == 0) {
    int rc = git_libgit2_init();
    if (rc < 0) {
      VXCORE_LOG_ERROR("git_libgit2_init() failed: rc=%d", rc);
      g_libgit2_initialized.store(false);
    } else {
      g_libgit2_initialized.store(true);
    }
    // Trigger the dead-strip anchor exactly once. Safe if the hook is null
    // (test binaries that don't link git_sync_backend.cpp).
    auto anchor = g_git_backend_link_anchor.load();
    if (anchor != nullptr) {
      anchor();
    }
  }
}

LibGit2Init::~LibGit2Init() {
  std::lock_guard<std::mutex> lock(g_libgit2_init_mutex);
  if (g_libgit2_refcount.fetch_sub(1) == 1) {
    git_libgit2_shutdown();
  }
}

bool LibGit2Init::ok() {
  return g_libgit2_initialized.load();
}

}  // namespace vxcore
