#ifndef VXCORE_WORK_QUEUE_H
#define VXCORE_WORK_QUEUE_H

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "vxcore/vxcore_types.h"

namespace vxcore {

using WorkItem = std::function<void()>;

class WorkQueue {
 public:
  VXCORE_API WorkQueue();
  VXCORE_API ~WorkQueue();

  WorkQueue(const WorkQueue &) = delete;
  WorkQueue &operator=(const WorkQueue &) = delete;

  VXCORE_API bool Enqueue(WorkItem item);

  VXCORE_API bool ProcessNext(int timeout_ms);

  VXCORE_API int ProcessAll();

  VXCORE_API size_t Size() const;

  VXCORE_API void Shutdown();

  VXCORE_API bool IsShutdown() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<WorkItem> queue_;
  bool shutdown_ = false;
};

class WorkQueueManager {
 public:
  VXCORE_API WorkQueueManager();
  VXCORE_API ~WorkQueueManager();

  WorkQueueManager(const WorkQueueManager &) = delete;
  WorkQueueManager &operator=(const WorkQueueManager &) = delete;

  VXCORE_API WorkQueue *GetOrCreate(const std::string &name);

  VXCORE_API WorkQueue *Get(const std::string &name) const;

  VXCORE_API void ShutdownAll();

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::unique_ptr<WorkQueue>> queues_;
};

}  // namespace vxcore

#endif  // VXCORE_WORK_QUEUE_H
