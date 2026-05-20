// Tests for ICredentialProvider, InMemoryCredentialProvider (Wave 6.1 / F4.4),
// and MockCredentialProvider (test_internals).

#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

#include "sync/credential_provider.h"
#include "sync/sync_types.h"
#include "test_internals/mock_credential_provider.h"
#include "test_utils.h"

namespace {

int test_in_memory_returns_creds() {
  std::cout << "  Running test_in_memory_returns_creds..." << std::endl;

  vxcore::SyncCredentials original;
  original.personal_access_token = "ghp_abcdef1234567890";
  original.author_name = "Alice";
  original.author_email = "alice@example.com";

  vxcore::InMemoryCredentialProvider provider(original);

  vxcore::SyncCredentials out;
  bool ok = provider.GetCredentials("https://github.com/user/repo.git", "x-access-token", &out);
  ASSERT_TRUE(ok);
  ASSERT_EQ(out.personal_access_token, "ghp_abcdef1234567890");
  ASSERT_EQ(out.author_name, "Alice");
  ASSERT_EQ(out.author_email, "alice@example.com");

  // Null-out pointer must return false.
  bool ok_null = provider.GetCredentials("https://github.com/user/repo.git", "x-access-token",
                                         nullptr);
  ASSERT_FALSE(ok_null);

  std::cout << "  PASS test_in_memory_returns_creds" << std::endl;
  return 0;
}

int test_mock_records_call() {
  std::cout << "  Running test_mock_records_call..." << std::endl;

  vxcore::MockCredentialProvider mock;
  ASSERT_EQ(mock.GetCallCount(), static_cast<std::size_t>(0));

  vxcore::SyncCredentials out;
  bool ok = mock.GetCredentials("https://example.com/repo.git", "user42", &out);
  ASSERT_TRUE(ok);
  ASSERT_EQ(mock.GetCallCount(), static_cast<std::size_t>(1));
  ASSERT_EQ(mock.GetLastUrl(), "https://example.com/repo.git");
  ASSERT_EQ(mock.GetLastUsername(), "user42");
  ASSERT_EQ(out.personal_access_token, "test-pat-token");

  // Configure to return false; caller sees false and call count still ticks.
  mock.SetAvailable(false);
  ok = mock.GetCredentials("https://example.com/repo2.git", "anon", &out);
  ASSERT_FALSE(ok);
  ASSERT_EQ(mock.GetCallCount(), static_cast<std::size_t>(2));
  ASSERT_EQ(mock.GetLastUrl(), "https://example.com/repo2.git");

  // Null-out also returns false and does NOT record (early return before lock).
  bool ok_null = mock.GetCredentials("https://example.com/repo.git", "user", nullptr);
  ASSERT_FALSE(ok_null);

  std::cout << "  PASS test_mock_records_call" << std::endl;
  return 0;
}

int test_concurrent_get_safe() {
  std::cout << "  Running test_concurrent_get_safe..." << std::endl;

  vxcore::SyncCredentials original;
  original.personal_access_token = "ghp_concurrency_test_pat_value_xyz";
  original.author_name = "Concurrent Author";
  original.author_email = "concurrent@example.com";

  vxcore::InMemoryCredentialProvider provider(original);

  constexpr int kThreadCount = 8;
  constexpr int kIterations = 1000;
  std::atomic<int> failures{0};
  std::atomic<int> successes{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int t = 0; t < kThreadCount; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < kIterations; ++i) {
        vxcore::SyncCredentials out;
        bool ok = provider.GetCredentials("https://example.com/repo.git",
                                          "x-access-token", &out);
        if (!ok) {
          failures.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        // Assert no torn reads: every snapshot must exactly equal original.
        if (out.personal_access_token != "ghp_concurrency_test_pat_value_xyz" ||
            out.author_name != "Concurrent Author" ||
            out.author_email != "concurrent@example.com") {
          failures.fetch_add(1, std::memory_order_relaxed);
        } else {
          successes.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }

  ASSERT_EQ(failures.load(), 0);
  ASSERT_EQ(successes.load(), kThreadCount * kIterations);

  std::cout << "  PASS test_concurrent_get_safe (" << successes.load()
            << " successful snapshots across " << kThreadCount << " threads)" << std::endl;
  return 0;
}

}  // namespace

// Test-mode shim: this test links vxcore.dll for InMemoryCredentialProvider
// and vxcore_test_internals for MockCredentialProvider. Neither touches the
// filesystem; vxcore_set_test_mode is called for consistency with the
// vxcore standalone-test pattern.
int main() {
  std::cout << "Running ICredentialProvider tests..." << std::endl;

  int result = 0;
  RUN_TEST(test_in_memory_returns_creds);
  RUN_TEST(test_mock_records_call);
  RUN_TEST(test_concurrent_get_safe);

  if (result == 0) {
    std::cout << "All tests passed!" << std::endl;
  }
  return result;
}
