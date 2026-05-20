#include "sync/credential_provider.h"

#include <utility>

namespace vxcore {

bool NoOpCredentialProvider::GetCredentials(const std::string &url,
                                            const std::string &username_from_url,
                                            SyncCredentials *out) {
  // Stateless: always declines. The C ABI uses this when the caller passes
  // a null credentials_json to vxcore_sync_enable, preserving the legacy
  // creds-less code path.
  (void)url;
  (void)username_from_url;
  (void)out;
  return false;
}

InMemoryCredentialProvider::InMemoryCredentialProvider(SyncCredentials creds)
    : creds_(std::move(creds)) {}

bool InMemoryCredentialProvider::GetCredentials(const std::string &url,
                                                const std::string &username_from_url,
                                                SyncCredentials *out) {
  // Per spec must-not: "Allow null-out pointer".
  if (out == nullptr) {
    return false;
  }
  // URL-agnostic provider; parameters intentionally unused.
  (void)url;
  (void)username_from_url;

  // Hold mutex only across the snapshot copy. Do not call out into caller
  // memory or any other vxcore subsystem while holding the lock.
  std::lock_guard<std::mutex> lock(mu_);
  *out = creds_;
  return true;
}

}  // namespace vxcore
