#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>

#include "mock_sync_backend.h"
#include "sync/sync_backend.h"
#include "sync/sync_types.h"
#include "test_git_sync_helpers.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_types.h"

using namespace vxcore;

int test_sync_credentials_default_fields() {
  std::cout << "  Running test_sync_credentials_default_fields..." << std::endl;
  SyncCredentials c;
  ASSERT_TRUE(c.personal_access_token.empty());
  ASSERT_TRUE(c.author_name.empty());
  ASSERT_TRUE(c.author_email.empty());
  std::cout << "  \xE2\x9C\x93 test_sync_credentials_default_fields passed" << std::endl;
  return 0;
}

int test_isync_backend_default_setcredentials_returns_ok() {
  std::cout << "  Running test_isync_backend_default_setcredentials_returns_ok..." << std::endl;
  MockSyncBackend mb;
  ASSERT_EQ(mb.SetCredentials(SyncCredentials{}), VXCORE_OK);
  std::cout << "  \xE2\x9C\x93 test_isync_backend_default_setcredentials_returns_ok passed"
            << std::endl;
  return 0;
}

int test_mock_sync_backend_records() {
  std::cout << "  Running test_mock_sync_backend_records..." << std::endl;
  MockSyncBackend mb;

  ASSERT_EQ(mb.Sync(nullptr, nullptr), VXCORE_OK);
  ASSERT_EQ(mb.sync_call_count, 1);

  ASSERT_EQ(mb.Push(nullptr, nullptr), VXCORE_OK);
  ASSERT_EQ(mb.push_call_count, 1);

  SyncCredentials creds;
  creds.personal_access_token = "abc";
  ASSERT_EQ(mb.SetCredentials(creds), VXCORE_OK);
  ASSERT_EQ(mb.set_credentials_call_count, 1);
  ASSERT_TRUE(mb.last_credentials.personal_access_token == "abc");

  std::cout << "  \xE2\x9C\x93 test_mock_sync_backend_records passed" << std::endl;
  return 0;
}

int test_helper_create_bare_repo() {
  std::cout << "  Running test_helper_create_bare_repo..." << std::endl;
  std::string bare_path = get_test_path("test_helper_bare");
  std::string url = vxcore_test::create_bare_repo(bare_path);
  ASSERT_TRUE(url.rfind("file:///", 0) == 0);
  ASSERT_TRUE(std::filesystem::exists(std::filesystem::u8path(bare_path + "/HEAD")));
  std::cout << "  \xE2\x9C\x93 test_helper_create_bare_repo passed" << std::endl;
  return 0;
}

int test_helper_commit_file_and_head_sha() {
  std::cout << "  Running test_helper_commit_file_and_head_sha..." << std::endl;
  std::string bare_path = get_test_path("test_helper_commit_bare");
  vxcore_test::create_bare_repo(bare_path);
  vxcore_test::commit_file(bare_path, "note.md", "hello", "first commit");
  std::string sha = vxcore_test::git_head_sha(bare_path);
  ASSERT_EQ(sha.length(), static_cast<size_t>(40));
  for (char c : sha) {
    ASSERT_TRUE(std::isxdigit(static_cast<unsigned char>(c)) != 0);
  }
  std::cout << "  \xE2\x9C\x93 test_helper_commit_file_and_head_sha passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running sync manager dispatch tests...\n";
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();
  RUN_TEST(test_sync_credentials_default_fields);
  RUN_TEST(test_isync_backend_default_setcredentials_returns_ok);
  RUN_TEST(test_mock_sync_backend_records);
  RUN_TEST(test_helper_create_bare_repo);
  RUN_TEST(test_helper_commit_file_and_head_sha);
  std::cout << "All sync manager dispatch tests passed\n";
  return 0;
}
