#include "sync_progress_dispatcher.h"

#include <utility>

namespace vxcore {

SyncProgressDispatcher::ObserverId SyncProgressDispatcher::Register(Observer observer) {
  if (!observer) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(observers_mutex_);
  const ObserverId id = next_id_++;
  observers_.emplace_back(id, std::move(observer));
  return id;
}

void SyncProgressDispatcher::Unregister(ObserverId id) {
  if (id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(observers_mutex_);
  for (auto it = observers_.begin(); it != observers_.end(); ++it) {
    if (it->first == id) {
      observers_.erase(it);
      return;
    }
  }
}

void SyncProgressDispatcher::Dispatch(const SyncProgress &event) const {
  // Wave 0.5: copy under lock, release, invoke. Observers may legally
  // call Register/Unregister/Dispatch reentrantly without deadlock and
  // without affecting the in-progress dispatch (copy semantics).
  std::vector<std::pair<ObserverId, Observer>> snapshot;
  {
    std::lock_guard<std::mutex> lock(observers_mutex_);
    snapshot = observers_;
  }
  for (const auto &kv : snapshot) {
    // kv.second is the std::function copied by value; safe to invoke.
    kv.second(event);
  }
}

std::size_t SyncProgressDispatcher::ObserverCountForTesting() const {
  std::lock_guard<std::mutex> lock(observers_mutex_);
  return observers_.size();
}

}  // namespace vxcore
