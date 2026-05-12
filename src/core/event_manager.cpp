#include "core/event_manager.h"

#include "core/work_queue.h"

namespace vxcore {

EventManager::EventManager() = default;

EventManager::~EventManager() = default;

EventManager::ListenerId EventManager::Subscribe(const std::string &event_name,
                                                  EventCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  ListenerId id = next_id_++;
  listeners_[event_name].push_back({id, std::move(callback)});
  return id;
}

void EventManager::Unsubscribe(ListenerId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &pair : listeners_) {
    auto &vec = pair.second;
    for (auto it = vec.begin(); it != vec.end(); ++it) {
      if (it->id == id) {
        vec.erase(it);
        return;
      }
    }
  }
}

void EventManager::Emit(const std::string &event_name, const nlohmann::json &data) {
  std::vector<EventCallback> callbacks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = listeners_.find(event_name);
    if (it == listeners_.end()) return;
    for (const auto &listener : it->second) {
      callbacks.push_back(listener.callback);
    }
  }
  for (const auto &cb : callbacks) {
    cb(event_name, data);
  }
}

void EventManager::EmitAsync(const std::string &event_name, const nlohmann::json &data,
                              WorkQueue *queue) {
  if (!queue) {
    Emit(event_name, data);
    return;
  }
  std::vector<EventCallback> callbacks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = listeners_.find(event_name);
    if (it == listeners_.end()) return;
    for (const auto &listener : it->second) {
      callbacks.push_back(listener.callback);
    }
  }
  // Capture by value so the lambda is self-contained
  queue->Enqueue([callbacks = std::move(callbacks), event_name, data] {
    for (const auto &cb : callbacks) {
      cb(event_name, data);
    }
  });
}

}  // namespace vxcore
