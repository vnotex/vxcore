#include "core/work_queue.h"

namespace vxcore {

WorkQueue::WorkQueue() = default;

WorkQueue::~WorkQueue() { Shutdown(); }

bool WorkQueue::Enqueue(WorkItem item) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return false;
    queue_.push_back(std::move(item));
  }
  cv_.notify_one();
  return true;
}

bool WorkQueue::ProcessNext(int timeout_ms) {
  WorkItem item;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (timeout_ms <= 0) {
      cv_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
    } else {
      cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                   [this] { return !queue_.empty() || shutdown_; });
    }
    if (queue_.empty()) return false;
    item = std::move(queue_.front());
    queue_.pop_front();
  }
  item();
  return true;
}

int WorkQueue::ProcessAll() {
  std::deque<WorkItem> batch;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    batch.swap(queue_);
  }
  for (auto &item : batch) {
    item();
  }
  return static_cast<int>(batch.size());
}

size_t WorkQueue::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

void WorkQueue::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
  }
  cv_.notify_all();
}

bool WorkQueue::IsShutdown() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return shutdown_;
}

WorkQueueManager::WorkQueueManager() = default;

WorkQueueManager::~WorkQueueManager() { ShutdownAll(); }

WorkQueue *WorkQueueManager::GetOrCreate(const std::string &name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = queues_.find(name);
  if (it != queues_.end()) return it->second.get();
  auto q = std::make_unique<WorkQueue>();
  auto *ptr = q.get();
  queues_[name] = std::move(q);
  return ptr;
}

WorkQueue *WorkQueueManager::Get(const std::string &name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = queues_.find(name);
  if (it != queues_.end()) return it->second.get();
  return nullptr;
}

void WorkQueueManager::ShutdownAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &pair : queues_) {
    pair.second->Shutdown();
  }
}

}  // namespace vxcore
