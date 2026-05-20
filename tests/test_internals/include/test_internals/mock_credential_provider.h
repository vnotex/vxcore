#ifndef VXCORE_TEST_INTERNALS_MOCK_CREDENTIAL_PROVIDER_H
#define VXCORE_TEST_INTERNALS_MOCK_CREDENTIAL_PROVIDER_H

#include <mutex>
#include <string>
#include <vector>

#include "sync/credential_provider.h"
#include "sync/sync_types.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

// MockCredentialProvider — test double for ICredentialProvider (Wave 6.1
// replacement for the original Wave 0.2 placeholder).
//
// Configurable returns + call recording mirror the MockSyncBackend pattern.
//
// Usage:
//   MockCredentialProvider provider;  // default creds, SetAvailable(true)
//   SyncCredentials out;
//   bool ok = provider.GetCredentials("https://example/repo.git",
//                                     "x-access-token", &out);
//   ASSERT_TRUE(ok);
//   ASSERT_EQ(provider.GetCallCount(), 1);
//   ASSERT_EQ(provider.GetLastUrl(), "https://example/repo.git");
//
// Threading: the same self-contained-mutex contract as
// InMemoryCredentialProvider. Safe to call from multiple threads.
class MockCredentialProvider : public ICredentialProvider {
 public:
  MockCredentialProvider();
  ~MockCredentialProvider() override;

  // ICredentialProvider:
  bool GetCredentials(const std::string &url,
                      const std::string &username_from_url,
                      SyncCredentials *out) override;

  // Configuration
  void SetCredentials(const SyncCredentials &creds);
  void SetAvailable(bool available);

  // Call recording
  std::size_t GetCallCount() const;
  std::string GetLastUrl() const;
  std::string GetLastUsername() const;

 private:
  mutable std::mutex mu_;
  SyncCredentials credentials_;
  bool available_ = true;
  std::size_t call_count_ = 0;
  std::string last_url_;
  std::string last_username_;
};

}  // namespace vxcore

#endif  // VXCORE_TEST_INTERNALS_MOCK_CREDENTIAL_PROVIDER_H
