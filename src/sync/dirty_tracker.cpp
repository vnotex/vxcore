#include "sync/dirty_tracker.h"

#include <utility>

namespace vxcore {

void DirtyTracker::MarkDirty(const std::string &notebookId, const std::string &path) {
  std::lock_guard<std::mutex> lock(mu_);
  dirty_[notebookId].insert(path);
}

std::vector<std::string> DirtyTracker::TakeDirty(const std::string &notebookId) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = dirty_.find(notebookId);
  if (it == dirty_.end()) {
    return {};
  }
  std::vector<std::string> out;
  out.reserve(it->second.size());
  for (auto &p : it->second) {
    out.push_back(p);
  }
  dirty_.erase(it);
  return out;
}

bool DirtyTracker::HasDirty(const std::string &notebookId) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = dirty_.find(notebookId);
  return it != dirty_.end() && !it->second.empty();
}

void DirtyTracker::Clear(const std::string &notebookId) {
  std::lock_guard<std::mutex> lock(mu_);
  dirty_.erase(notebookId);
}

void DirtyTracker::ClearAll() {
  std::lock_guard<std::mutex> lock(mu_);
  dirty_.clear();
}

}  // namespace vxcore
