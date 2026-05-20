#ifndef VXCORE_CREDENTIAL_PROVIDER_H
#define VXCORE_CREDENTIAL_PROVIDER_H

#include <mutex>
#include <string>

#include "sync/sync_types.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

// ICredentialProvider — abstract interface for supplying SyncCredentials to
// sync backends (e.g., the libgit2 credential callback).
//
// Threading contract:
//   GetCredentials() is invoked from the libgit2 callback thread which holds
//   NO vxcore mutex (see libs/vxcore/src/sync/AGENTS.md § Threading &
//   Callback Contract: "callbacks must not fire while holding any sync
//   mutex"). Implementations MUST be thread-safe. Any internal mutex held by
//   an implementation is self-contained — it MUST NOT nest with any other
//   vxcore sync mutex.
//
// Snapshot semantics:
//   GetCredentials() writes a COPY of the current credentials into *out and
//   returns. The caller owns *out; subsequent provider updates do NOT mutate
//   already-returned snapshots.
class VXCORE_API ICredentialProvider {
 public:
  virtual ~ICredentialProvider() = default;

  // Returns true if credentials were written into *out; false otherwise.
  // Implementations MUST return false when out == nullptr.
  // Called from libgit2 callback thread; MUST be thread-safe.
  virtual bool GetCredentials(const std::string &url,
                              const std::string &username_from_url,
                              SyncCredentials *out) = 0;
};

// NoOpCredentialProvider — stateless provider that always returns false.
// Used when the caller declares no credentials are needed (local sync) or
// wants to defer to the system git credential helper / a backend that
// handles absent credentials gracefully. Stateless; safe to share across
// threads. No mutex (no internal state to protect).
//
// Wave 7.1 (sync-backend-phase4): The unified vxcore_sync_enable accepts
// a nullable credentials_json. When null, the C ABI constructs a
// NoOpCredentialProvider and forwards it to SyncManager::EnableSync, which
// preserves the legacy creds-less code path while satisfying the
// non-null-provider contract documented on EnableSync.
class VXCORE_API NoOpCredentialProvider : public ICredentialProvider {
 public:
  NoOpCredentialProvider() = default;
  ~NoOpCredentialProvider() override = default;

  bool GetCredentials(const std::string &url,
                      const std::string &username_from_url,
                      SyncCredentials *out) override;
};

// InMemoryCredentialProvider — URL-agnostic provider that returns a
// fixed-at-construction SyncCredentials snapshot under a self-contained
// mutex. The mutex is held only across the copy into *out; it does not
// nest with any other vxcore sync mutex.
class VXCORE_API InMemoryCredentialProvider : public ICredentialProvider {
 public:
  explicit InMemoryCredentialProvider(SyncCredentials creds);
  ~InMemoryCredentialProvider() override = default;

  bool GetCredentials(const std::string &url,
                      const std::string &username_from_url,
                      SyncCredentials *out) override;

 private:
  mutable std::mutex mu_;
  SyncCredentials creds_;
};

}  // namespace vxcore

#endif  // VXCORE_CREDENTIAL_PROVIDER_H
