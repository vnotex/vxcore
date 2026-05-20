#ifndef VXCORE_TEST_INTERNALS_MOCK_CREDENTIAL_PROVIDER_H
#define VXCORE_TEST_INTERNALS_MOCK_CREDENTIAL_PROVIDER_H

#include <string>

#include "sync/sync_types.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

// PLACEHOLDER: ICredentialProvider interface
//
// This is a temporary placeholder interface for testing purposes.
// The real ICredentialProvider interface will be defined in Wave 6.1.
// Once Wave 6.1 lands, this placeholder will be replaced with the real interface.
//
// TODO: REPLACE WITH REAL ICredentialProvider IN WAVE 6.1
class ICredentialProviderPlaceholder {
 public:
  virtual ~ICredentialProviderPlaceholder() = default;

  // Get credentials for the given notebook ID.
  // Returns true if credentials are available, false otherwise.
  virtual bool GetCredentials(const std::string &notebook_id,
                              SyncCredentials &out_creds) = 0;
};

// MockCredentialProvider — minimal stub implementation for testing.
//
// Returns a fixed SyncCredentials struct with a test PAT.
// Used by tests that need credential injection without the real credential
// provider infrastructure.
//
// Usage:
//   MockCredentialProvider provider;
//   SyncCredentials creds;
//   bool ok = provider.GetCredentials("test-notebook", creds);
//   ASSERT_TRUE(ok);
//   ASSERT_EQ(creds.personal_access_token, "test-pat-token");
class MockCredentialProvider : public ICredentialProviderPlaceholder {
 public:
  MockCredentialProvider();
  ~MockCredentialProvider() override;

  bool GetCredentials(const std::string &notebook_id,
                      SyncCredentials &out_creds) override;

  // Configuration methods
  void SetCredentials(const SyncCredentials &creds);
  void SetAvailable(bool available);

 private:
  SyncCredentials credentials_;
  bool available_ = true;
};

}  // namespace vxcore

#endif  // VXCORE_TEST_INTERNALS_MOCK_CREDENTIAL_PROVIDER_H
