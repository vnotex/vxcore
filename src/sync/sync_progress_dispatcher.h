#ifndef VXCORE_SYNC_PROGRESS_DISPATCHER_H
#define VXCORE_SYNC_PROGRESS_DISPATCHER_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "sync_types.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

// Thread-safe fan-out of SyncProgress events to multiple observers.
//
// Wave 13.1 (F5.7 part 2). Locking contract (Wave 0.5):
// * Register/Unregister take observers_mutex_ briefly to mutate the list.
// * Dispatch takes observers_mutex_ to COPY the observer list, RELEASES,
//   then invokes each observer outside the lock. This is mandatory — an
//   observer must never run with our mutex held.
// * If an observer registers/unregisters during dispatch (because it lands
//   on this thread reentrantly, OR another thread does it), the change is
//   applied AFTER the current dispatch finishes (copy semantics give that
//   for free — we iterate the copy, not the live list).
//
// The dispatcher is INTERNAL to the vxcore DLL (no VXCORE_API on the type
// itself; SyncManager exposes register/unregister via its own VXCORE_API).
class SyncProgressDispatcher {
 public:
  using ObserverId = std::uint64_t;
  using Observer = std::function<void(const SyncProgress &)>;

  SyncProgressDispatcher() = default;
  ~SyncProgressDispatcher() = default;

  SyncProgressDispatcher(const SyncProgressDispatcher &) = delete;
  SyncProgressDispatcher &operator=(const SyncProgressDispatcher &) = delete;

  // Returns the observer ID; pass it to Unregister to remove.
  // Returns 0 if the observer is empty (callable target is nullptr-like).
  ObserverId Register(Observer observer);

  // No-op if id is unknown or 0.
  void Unregister(ObserverId id);

  // Copies the observer list under lock, releases, then invokes each outside
  // the lock. Safe to call concurrently with Register / Unregister / other
  // Dispatch invocations.
  void Dispatch(const SyncProgress &event) const;

  // Test-only helper: number of currently registered observers.
  std::size_t ObserverCountForTesting() const;

 private:
  mutable std::mutex observers_mutex_;
  std::vector<std::pair<ObserverId, Observer>> observers_;
  ObserverId next_id_ = 1;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_PROGRESS_DISPATCHER_H
