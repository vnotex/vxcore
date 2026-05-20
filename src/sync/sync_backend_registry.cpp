#include "sync/sync_backend_registry.h"

#include <algorithm>
#include <utility>

namespace vxcore {

SyncBackendRegistry &SyncBackendRegistry::Instance() {
  // Meyer's singleton — C++11 guarantees thread-safe local-static init.
  static SyncBackendRegistry instance;
  return instance;
}

bool SyncBackendRegistry::Register(const std::string &name,
                                   SyncBackendFactory factory) {
  if (name.empty() || !factory) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  // First-wins: do NOT overwrite an existing entry.
  auto it = factories_.find(name);
  if (it != factories_.end()) {
    return false;
  }
  factories_.emplace(name, std::move(factory));
  return true;
}

std::unique_ptr<ISyncBackend> SyncBackendRegistry::Create(
    const std::string &name, const SyncConfig &cfg) const {
  // Copy the factory out under the lock, then release the lock before
  // invoking — respect the no-callback-under-mutex contract.
  SyncBackendFactory factory_copy;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = factories_.find(name);
    if (it == factories_.end()) {
      return nullptr;
    }
    factory_copy = it->second;
  }
  if (!factory_copy) {
    return nullptr;
  }
  return factory_copy(cfg);
}

std::vector<std::string> SyncBackendRegistry::Names() const {
  std::vector<std::string> names;
  {
    std::lock_guard<std::mutex> lock(mu_);
    names.reserve(factories_.size());
    for (const auto &kv : factories_) {
      names.push_back(kv.first);
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

BackendRegistration::BackendRegistration(const std::string &name,
                                         SyncBackendFactory factory) {
  // Static-init exception safety: swallow ANY exception so program startup
  // is never aborted by a stray throw from inside the std::function copy or
  // the unordered_map insertion. A failed registration is a silent no-op.
  try {
    (void)SyncBackendRegistry::Instance().Register(name, std::move(factory));
  } catch (...) {
    // intentionally swallowed
  }
}

}  // namespace vxcore
