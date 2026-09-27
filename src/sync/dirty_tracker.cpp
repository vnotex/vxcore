#include "sync/dirty_tracker.h"

#include <utility>

namespace vxcore {

void DirtyTracker::MarkDirty(const std::string &notebookId, const std::string &path) {
  std::lock_guard<std::mutex> lock(mu_);
  auto &state = dirty_[notebookId];
  ++state.revision;
  state.paths.insert(path);
}

std::vector<std::string> DirtyTracker::TakeDirty(const std::string &notebookId) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = dirty_.find(notebookId);
  if (it == dirty_.end()) {
    return {};
  }
  std::vector<std::string> out;
  out.reserve(it->second.paths.size());
  for (const auto &p : it->second.paths) {
    out.push_back(p);
  }
  it->second.paths.clear();
  return out;
}

bool DirtyTracker::HasDirty(const std::string &notebookId) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = dirty_.find(notebookId);
  return it != dirty_.end() && !it->second.paths.empty();
}

void DirtyTracker::Clear(const std::string &notebookId) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = dirty_.find(notebookId);
  if (it != dirty_.end()) it->second.paths.clear();
}

uint64_t DirtyTracker::Revision(const std::string &notebookId) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = dirty_.find(notebookId);
  return it == dirty_.end() ? 0 : it->second.revision;
}

bool DirtyTracker::ClearIfUnchanged(const std::string &notebookId, uint64_t revision) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = dirty_.find(notebookId);
  if (it == dirty_.end()) return revision == 0;
  if (it->second.revision != revision) return false;
  it->second.paths.clear();
  return true;
}

void DirtyTracker::ClearAll() {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto &entry : dirty_) entry.second.paths.clear();
}

std::vector<std::string> DirtyTracker::ListDirtyNotebooks() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> out;
  out.reserve(dirty_.size());
  for (const auto &kv : dirty_) {
    if (!kv.second.paths.empty()) {
      out.push_back(kv.first);
    }
  }
  return out;
}

}  // namespace vxcore
