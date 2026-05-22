#ifndef VXCORE_SYNC_DIRTY_TRACKER_H
#define VXCORE_SYNC_DIRTY_TRACKER_H

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vxcore {

// DirtyTracker keeps a per-notebook set of dirty paths.
//
// Thread-safety: all public methods are internally synchronized via mu_.
//
// Wave 0.5 contract: methods never invoke callbacks, never fire events, and
// never call into other subsystems while holding mu_. They return plain data
// so the caller can fan-out notifications outside the lock.
class DirtyTracker {
 public:
  DirtyTracker() = default;
  ~DirtyTracker() = default;

  DirtyTracker(const DirtyTracker &) = delete;
  DirtyTracker &operator=(const DirtyTracker &) = delete;

  // Mark `path` as dirty for `notebookId`. Idempotent (insert into a set).
  void MarkDirty(const std::string &notebookId, const std::string &path);

  // Return all dirty paths for `notebookId` and clear that notebook's entry.
  // Returns an empty vector if the notebook has no dirty paths.
  std::vector<std::string> TakeDirty(const std::string &notebookId);

  // Return true if `notebookId` has at least one dirty path tracked.
  bool HasDirty(const std::string &notebookId) const;

  // Drop all dirty paths for `notebookId` (no-op if not tracked).
  void Clear(const std::string &notebookId);

  // Drop all dirty paths for every notebook.
  void ClearAll();

  // Return a snapshot of all notebook IDs that currently have at least one
  // dirty path tracked. Order is unspecified (unordered_map iteration).
  // Used by SyncManager::GetDirtyNotebooks to enumerate the dirty set
  // without exposing DirtyTracker on the public API.
  std::vector<std::string> ListDirtyNotebooks() const;

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::unordered_set<std::string>> dirty_;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_DIRTY_TRACKER_H
