// F5.9 / Task 12.1 of sync-backend-phase4: cooperative cancellation token for
// in-flight sync operations.
//
// Design notes:
//   - `SyncCancellation` is a lock-free, atomic-only token. NO mutex, NO
//     condition variable. Callers MAY invoke `Cancel()` from any thread,
//     including the UI thread, without blocking.
//   - `Libgit2ProgressCheck` is the function-pointer shim used as libgit2's
//     `transfer_progress` callback (signature `git_indexer_progress_cb`).
//     Returns nonzero to abort the libgit2 op; libgit2 then surfaces the
//     abort as `GIT_EUSER` (-7).
//   - The token is OPT-IN. Pipelines that have not been given a token (or
//     whose token pointer is null) behave identically to pre-W12 code:
//     libgit2 callbacks never observe cancellation.
//
// libgit2 callback contract (`git_remote_callbacks.transfer_progress`):
//   int(*)(const git_indexer_progress *stats, void *payload)
// In libgit2 1.7.x `git_transfer_progress` is a deprecated typedef for
// `git_indexer_progress` (see third_party/libgit2/include/git2/deprecated.h
// line 831), so the plan-spec name `git_transfer_progress` and the modern
// `git_indexer_progress` are the same struct. We forward-declare the modern
// name to avoid pulling <git2.h> into this header.
//
// Threading rules (cross-reference src/sync/AGENTS.md § Threading & Callback
// Contract): libgit2 invokes the progress callback from its transport
// thread, NOT under any vxcore mutex. The atomic load in `IsCancelled()` is
// lock-free by definition (`std::atomic<bool>` is always lock-free on every
// supported platform). The cancellation token is never read or written
// while `SyncManager::state_mutex_` is held.

#ifndef VXCORE_SYNC_SYNC_CANCELLATION_H
#define VXCORE_SYNC_SYNC_CANCELLATION_H

#include <atomic>
#include <cstddef>
#include <memory>

// Forward declarations of the libgit2 progress structs. We do NOT include
// <git2.h> here -- only the .cpp pulls in libgit2 headers indirectly via
// the pointer-only signature.
struct git_indexer_progress;

namespace vxcore {

class SyncCancellation {
 public:
  SyncCancellation() = default;
  ~SyncCancellation() = default;

  SyncCancellation(const SyncCancellation &) = delete;
  SyncCancellation &operator=(const SyncCancellation &) = delete;
  SyncCancellation(SyncCancellation &&) = delete;
  SyncCancellation &operator=(SyncCancellation &&) = delete;

  // Mark this token as cancelled. Lock-free, idempotent, safe to call from
  // any thread. Stores with seq_cst memory ordering (the std::atomic default).
  void Cancel() noexcept;

  // Returns true once `Cancel()` has been called. Lock-free.
  bool IsCancelled() const noexcept;

  // libgit2 `transfer_progress_cb` shim (signature
  // `int(const git_indexer_progress*, void*)`). `token` MUST be a raw
  // `SyncCancellation*` (libgit2 holds a void* payload, not a shared_ptr).
  // Returns 0 to continue, nonzero (1) to abort.
  //
  // NEVER throws (libgit2 calls this from C; exceptions across a C ABI are
  // UB). Null `token` is safe and returns 0 -- this is the "no cancellation
  // wired" default.
  static int Libgit2ProgressCheck(const git_indexer_progress *stats,
                                  void *token) noexcept;

  // libgit2 `push_transfer_progress_cb` shim (different signature than the
  // fetch-side progress callback). Same semantics: read the cancellation
  // atom, return 0 to continue / 1 to abort. `payload` is a
  // `SyncCancellation*`. Null `payload` returns 0.
  static int Libgit2PushProgressCheck(unsigned int current,
                                      unsigned int total,
                                      std::size_t bytes,
                                      void *payload) noexcept;

 private:
  std::atomic<bool> cancelled_{false};
};

using SyncCancellationPtr = std::shared_ptr<SyncCancellation>;

}  // namespace vxcore

#endif  // VXCORE_SYNC_SYNC_CANCELLATION_H
