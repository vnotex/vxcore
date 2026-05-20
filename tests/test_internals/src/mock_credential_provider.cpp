#include "test_internals/mock_credential_provider.h"

namespace vxcore {

MockCredentialProvider::MockCredentialProvider() {
  credentials_.personal_access_token = "test-pat-token";
  credentials_.author_name = "Test Author";
  credentials_.author_email = "test@example.com";
}

MockCredentialProvider::~MockCredentialProvider() = default;

bool MockCredentialProvider::GetCredentials(const std::string &url,
                                            const std::string &username_from_url,
                                            SyncCredentials *out) {
  if (out == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  ++call_count_;
  last_url_ = url;
  last_username_ = username_from_url;
  if (!available_) {
    return false;
  }
  *out = credentials_;
  return true;
}

void MockCredentialProvider::SetCredentials(const SyncCredentials &creds) {
  std::lock_guard<std::mutex> lock(mu_);
  credentials_ = creds;
}

void MockCredentialProvider::SetAvailable(bool available) {
  std::lock_guard<std::mutex> lock(mu_);
  available_ = available;
}

std::size_t MockCredentialProvider::GetCallCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return call_count_;
}

std::string MockCredentialProvider::GetLastUrl() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_url_;
}

std::string MockCredentialProvider::GetLastUsername() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_username_;
}

}  // namespace vxcore
