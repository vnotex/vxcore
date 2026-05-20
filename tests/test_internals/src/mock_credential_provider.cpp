#include "test_internals/mock_credential_provider.h"

namespace vxcore {

MockCredentialProvider::MockCredentialProvider() {
  credentials_.personal_access_token = "test-pat-token";
  credentials_.author_name = "Test Author";
  credentials_.author_email = "test@example.com";
}

MockCredentialProvider::~MockCredentialProvider() = default;

bool MockCredentialProvider::GetCredentials(const std::string &notebook_id,
                                            SyncCredentials &out_creds) {
  (void)notebook_id;
  if (!available_) {
    return false;
  }
  out_creds = credentials_;
  return true;
}

void MockCredentialProvider::SetCredentials(const SyncCredentials &creds) {
  credentials_ = creds;
}

void MockCredentialProvider::SetAvailable(bool available) {
  available_ = available;
}

}  // namespace vxcore
