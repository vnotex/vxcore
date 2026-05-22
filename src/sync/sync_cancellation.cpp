// F5.9 / Task 12.1 of sync-backend-phase4: SyncCancellation implementation.
//
// Lock-free atomic operations only. Header forward-declares the libgit2
// progress structs; the .cpp does NOT need <git2.h> because the shims only
// dereference `token`/`payload` (a raw `SyncCancellation*`), never the
// stats pointer.

#include "sync/sync_cancellation.h"

namespace vxcore {

void SyncCancellation::Cancel() noexcept {
  // seq_cst is the std::atomic default; explicit here for clarity.
  cancelled_.store(true, std::memory_order_seq_cst);
}

bool SyncCancellation::IsCancelled() const noexcept {
  return cancelled_.load(std::memory_order_seq_cst);
}

int SyncCancellation::Libgit2ProgressCheck(const git_indexer_progress * /*stats*/,
                                            void *token) noexcept {
  // Defensive null-check: when the pipeline has no cancellation wired,
  // callers pass a null payload through libgit2 and we must not crash.
  if (token == nullptr) {
    return 0;
  }
  auto *self = static_cast<SyncCancellation *>(token);
  return self->IsCancelled() ? 1 : 0;
}

int SyncCancellation::Libgit2PushProgressCheck(unsigned int /*current*/,
                                                unsigned int /*total*/,
                                                std::size_t /*bytes*/,
                                                void *payload) noexcept {
  if (payload == nullptr) {
    return 0;
  }
  auto *self = static_cast<SyncCancellation *>(payload);
  return self->IsCancelled() ? 1 : 0;
}

}  // namespace vxcore
