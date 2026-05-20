// Smoke test for MockSyncBackend and MockCredentialProvider.
//
// Verifies:
// 1. Mock records method calls
// 2. Mock returns configured error codes
// 3. Mock can fake conflicts on demand

#include <iostream>

#include "test_internals/mock_credential_provider.h"
#include "test_internals/mock_sync_backend.h"
#include "test_utils.h"
#include "vxcore/vxcore_types.h"

namespace {

int test_record_call_count() {
  std::cout << "  Running test_record_call_count..." << std::endl;

  vxcore::MockSyncBackend mock;
  ASSERT_EQ(mock.GetCallCount(), 0);

  // Call Initialize
  vxcore::SyncConfig config;
  config.backend = "mock";
  config.remote_url = "test://repo";
  mock.Initialize("test_root", config);
  ASSERT_EQ(mock.GetCallCount(), 1);

  // Call Sync
  mock.Sync(nullptr, nullptr);
  ASSERT_EQ(mock.GetCallCount(), 2);

  // Call GetStatus
  std::vector<vxcore::SyncFileInfo> files;
  mock.GetStatus(files);
  ASSERT_EQ(mock.GetCallCount(), 3);

  // Verify call records
  const auto &records = mock.GetCallRecords();
  ASSERT_EQ(records.size(), 3);
  ASSERT_EQ(records[0], "Initialize");
  ASSERT_EQ(records[1], "Sync");
  ASSERT_EQ(records[2], "GetStatus");

  std::cout << "  ✓ test_record_call_count passed" << std::endl;
  return 0;
}

int test_return_configured_error() {
  std::cout << "  Running test_return_configured_error..." << std::endl;

  vxcore::MockSyncBackend mock;

  // Default: VXCORE_OK
  vxcore::SyncConfig config;
  VxCoreError err = mock.Initialize("test_root", config);
  ASSERT_EQ(err, VXCORE_OK);

  // Configure Sync to return error
  mock.SetReturnCode("Sync", VXCORE_ERR_SYNC_NETWORK);
  err = mock.Sync(nullptr, nullptr);
  ASSERT_EQ(err, VXCORE_ERR_SYNC_NETWORK);

  // Configure GetStatus to return error
  mock.SetReturnCode("GetStatus", VXCORE_ERR_SYNC_NOT_ENABLED);
  std::vector<vxcore::SyncFileInfo> files;
  err = mock.GetStatus(files);
  ASSERT_EQ(err, VXCORE_ERR_SYNC_NOT_ENABLED);

  // Verify call count still increments
  ASSERT_EQ(mock.GetCallCount(), 3);

  std::cout << "  ✓ test_return_configured_error passed" << std::endl;
  return 0;
}

int test_fake_conflicts() {
  std::cout << "  Running test_fake_conflicts..." << std::endl;

  vxcore::MockSyncBackend mock;

  // Without fake conflicts, Sync returns OK
  vxcore::SyncConfig config;
  mock.Initialize("test_root", config);
  VxCoreError err = mock.Sync(nullptr, nullptr);
  ASSERT_EQ(err, VXCORE_OK);

  // Enable fake conflicts
  mock.EnableFakeConflicts(true);
  err = mock.Sync(nullptr, nullptr);
  ASSERT_EQ(err, VXCORE_ERR_SYNC_CONFLICT);

  // Disable fake conflicts
  mock.EnableFakeConflicts(false);
  err = mock.Sync(nullptr, nullptr);
  ASSERT_EQ(err, VXCORE_OK);

  std::cout << "  ✓ test_fake_conflicts passed" << std::endl;
  return 0;
}

int test_mock_credential_provider() {
  std::cout << "  Running test_mock_credential_provider..." << std::endl;

  vxcore::MockCredentialProvider provider;

  // Get default credentials
  vxcore::SyncCredentials creds;
  bool ok = provider.GetCredentials("test-notebook", creds);
  ASSERT_TRUE(ok);
  ASSERT_EQ(creds.personal_access_token, "test-pat-token");
  ASSERT_EQ(creds.author_name, "Test Author");
  ASSERT_EQ(creds.author_email, "test@example.com");

  // Set custom credentials
  vxcore::SyncCredentials custom;
  custom.personal_access_token = "custom-token";
  custom.author_name = "Custom Author";
  custom.author_email = "custom@example.com";
  provider.SetCredentials(custom);

  creds = vxcore::SyncCredentials();
  ok = provider.GetCredentials("test-notebook", creds);
  ASSERT_TRUE(ok);
  ASSERT_EQ(creds.personal_access_token, "custom-token");
  ASSERT_EQ(creds.author_name, "Custom Author");

  // Disable credentials
  provider.SetAvailable(false);
  ok = provider.GetCredentials("test-notebook", creds);
  ASSERT_FALSE(ok);

  std::cout << "  ✓ test_mock_credential_provider passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  std::cout << "Running MockSyncBackend and MockCredentialProvider tests..."
            << std::endl;

  int result = 0;
  result |= test_record_call_count();
  result |= test_return_configured_error();
  result |= test_fake_conflicts();
  result |= test_mock_credential_provider();

  if (result == 0) {
    std::cout << "All tests passed!" << std::endl;
  }
  return result;
}
