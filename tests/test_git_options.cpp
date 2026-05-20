// Task 5.4 (F1.5) of sync-backend-phase4: GitOptions::FromJson contract tests.
//
// Pure value-type tests — no libgit2, no SyncManager, no GitSyncBackend
// instantiation. Linked via add_vxcore_test() against vxcore.dll, which
// re-exports GitOptions::FromJson via VXCORE_API.

#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "sync/git/git_options.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace {

using vxcore::GitOptions;

int defaults_when_json_empty() {
  std::cout << "  Running defaults_when_json_empty..." << std::endl;
  // Three flavors of "no input" must all yield defaults.
  for (const auto &j :
       {nlohmann::json{}, nlohmann::json::object(), nlohmann::json(nullptr)}) {
    GitOptions o = GitOptions::FromJson(j);
    ASSERT_EQ(o.ssl_verify, true);
    ASSERT_EQ(o.connect_timeout_ms, 30000);
    ASSERT_TRUE(o.proxy_url.empty());
  }
  std::cout << "  PASS" << std::endl;
  return 0;
}

int partial_json_overrides_only_present_fields() {
  std::cout << "  Running partial_json_overrides_only_present_fields..." << std::endl;
  nlohmann::json j = nlohmann::json::object();
  j["sslVerify"] = false;
  GitOptions o = GitOptions::FromJson(j);
  ASSERT_EQ(o.ssl_verify, false);              // overridden
  ASSERT_EQ(o.connect_timeout_ms, 30000);      // default kept
  ASSERT_TRUE(o.proxy_url.empty());            // default kept
  std::cout << "  PASS" << std::endl;
  return 0;
}

int full_json_overrides_all_fields() {
  std::cout << "  Running full_json_overrides_all_fields..." << std::endl;
  nlohmann::json j = nlohmann::json::object();
  j["sslVerify"] = false;
  j["connectTimeoutMs"] = 12345;
  j["proxyUrl"] = "http://proxy.example.com:8080";
  GitOptions o = GitOptions::FromJson(j);
  ASSERT_EQ(o.ssl_verify, false);
  ASSERT_EQ(o.connect_timeout_ms, 12345);
  ASSERT_EQ(o.proxy_url, std::string("http://proxy.example.com:8080"));
  std::cout << "  PASS" << std::endl;
  return 0;
}

int malformed_json_falls_back_to_defaults() {
  std::cout << "  Running malformed_json_falls_back_to_defaults..." << std::endl;
  nlohmann::json j = nlohmann::json::object();
  // Wrong types: string where bool expected, string where int expected,
  // number where string expected. None must throw; each must fall back.
  j["sslVerify"] = "yes";
  j["connectTimeoutMs"] = "30s";
  j["proxyUrl"] = 42;
  GitOptions o = GitOptions::FromJson(j);
  ASSERT_EQ(o.ssl_verify, true);
  ASSERT_EQ(o.connect_timeout_ms, 30000);
  ASSERT_TRUE(o.proxy_url.empty());

  // Also exercise a non-object top-level — array, scalar — to confirm the
  // is_object() guard. Should not throw.
  GitOptions o2 = GitOptions::FromJson(nlohmann::json::array({1, 2, 3}));
  ASSERT_EQ(o2.ssl_verify, true);
  GitOptions o3 = GitOptions::FromJson(nlohmann::json("oops"));
  ASSERT_EQ(o3.connect_timeout_ms, 30000);

  std::cout << "  PASS" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  std::cout << "Running GitOptions::FromJson tests..." << std::endl;
  RUN_TEST(defaults_when_json_empty);
  RUN_TEST(partial_json_overrides_only_present_fields);
  RUN_TEST(full_json_overrides_all_fields);
  RUN_TEST(malformed_json_falls_back_to_defaults);
  std::cout << "All GitOptions tests passed." << std::endl;
  return 0;
}
