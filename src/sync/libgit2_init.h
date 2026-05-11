#ifndef VXCORE_LIBGIT2_INIT_H
#define VXCORE_LIBGIT2_INIT_H
namespace vxcore {
// RAII wrapper for libgit2's process-global init/shutdown lifecycle.
// Multiple instances are safe (ref-counted internally). The first instance
// calls git_libgit2_init(); the last destroyed calls git_libgit2_shutdown().
// Construction is thread-safe.
class LibGit2Init {
 public:
  LibGit2Init();
  ~LibGit2Init();
  LibGit2Init(const LibGit2Init&) = delete;
  LibGit2Init& operator=(const LibGit2Init&) = delete;
  LibGit2Init(LibGit2Init&&) = delete;
  LibGit2Init& operator=(LibGit2Init&&) = delete;
};
}  // namespace vxcore
#endif  // VXCORE_LIBGIT2_INIT_H
