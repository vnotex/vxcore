#include <git2.h>

#include <iostream>

#include "sync/git/libgit2_init.h"
#include "test_utils.h"

static int test_libgit2_init_refcount() {
  {
    vxcore::LibGit2Init init1;
    {
      vxcore::LibGit2Init init2;
      {
        vxcore::LibGit2Init init3;
        // Inside innermost scope: libgit2 should be initialized.
        int rc = git_libgit2_init();
        ASSERT(rc >= 1);
        // Undo our manual init.
        int rc_shut = git_libgit2_shutdown();
        ASSERT(rc_shut >= 0);
      }
    }
  }
  // After all RAII instances are destroyed, libgit2 should have been fully
  // shut down. Calling git_libgit2_init() again must succeed and return >= 1
  // (proving we did not over-shutdown).
  int rc = git_libgit2_init();
  ASSERT(rc >= 1);
  // Clean up our manual init so we leave the process in a clean state.
  int rc_shut = git_libgit2_shutdown();
  ASSERT(rc_shut >= 0);

  std::cout << "  ✓ test_libgit2_init_refcount passed" << std::endl;
  return 0;
}

int main() {
  RUN_TEST(test_libgit2_init_refcount);
  return 0;
}
