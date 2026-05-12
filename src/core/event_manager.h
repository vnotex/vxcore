#ifndef VXCORE_EVENT_MANAGER_H
#define VXCORE_EVENT_MANAGER_H

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "vxcore/vxcore_types.h"

namespace vxcore {

class WorkQueue;

using EventCallback = std::function<void(const std::string &event_name, const nlohmann::json &data)>;

class EventManager {
 public:
  VXCORE_API EventManager();
  VXCORE_API ~EventManager();

  EventManager(const EventManager &) = delete;
  EventManager &operator=(const EventManager &) = delete;

  using ListenerId = uint64_t;

  VXCORE_API ListenerId Subscribe(const std::string &event_name, EventCallback callback);

  VXCORE_API void Unsubscribe(ListenerId id);

  VXCORE_API void Emit(const std::string &event_name, const nlohmann::json &data);

  VXCORE_API void EmitAsync(const std::string &event_name, const nlohmann::json &data,
                            WorkQueue *queue);

 private:
  struct Listener {
    ListenerId id;
    EventCallback callback;
  };

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::vector<Listener>> listeners_;
  ListenerId next_id_ = 1;
};

}  // namespace vxcore

#endif  // VXCORE_EVENT_MANAGER_H
