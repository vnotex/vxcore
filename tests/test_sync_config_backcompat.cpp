// Task 7.3 (F3.3): SyncConfig::enabled was removed.
//
// - Serializer must NOT emit the `"enabled"` key any more.
// - Deserializer must SILENTLY ignore legacy `"enabled"` keys (backward compat
//   with on-disk JSON written by older builds).
//
// This test exercises both contracts and links only against `vxcore`'s public
// API surface for `SyncConfig::FromJson`/`ToJson` — no backend wiring required.

#include <iostream>
#include <nlohmann/json.hpp>

#include "sync/sync_types.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace {

int test_enabled_field_absent_in_serialize() {
  std::cout << "  Running test_enabled_field_absent_in_serialize..." << std::endl;

  vxcore::SyncConfig cfg;
  cfg.backend = "git";
  cfg.remote_url = "https://example.invalid/repo.git";
  cfg.interval_seconds = 120;
  cfg.exclude_paths = {"*.tmp"};
  cfg.auto_commit_merges = false;

  const nlohmann::json j = cfg.ToJson();
  ASSERT_FALSE(j.contains("enabled"));

  // Sanity-check that the rest of the fields are still being emitted as before.
  ASSERT_TRUE(j.contains("backend"));
  ASSERT_TRUE(j.contains("remoteUrl"));
  ASSERT_TRUE(j.contains("intervalSeconds"));
  ASSERT_TRUE(j.contains("excludePaths"));
  ASSERT_TRUE(j.contains("autoCommitMerges"));

  // And that the serialized string itself never contains the substring
  // — guards against accidental re-introduction via a sibling key, e.g.
  // backendOptions["enabled"].
  const std::string dumped = j.dump();
  ASSERT_TRUE(dumped.find("\"enabled\"") == std::string::npos);

  std::cout << "  PASSED test_enabled_field_absent_in_serialize" << std::endl;
  return 0;
}

int test_enabled_field_ignored_in_deserialize() {
  std::cout << "  Running test_enabled_field_ignored_in_deserialize..." << std::endl;

  // Legacy on-disk shape: `enabled` is present but should be silently dropped.
  const std::string legacy =
      "{"
      "\"backend\":\"git\","
      "\"remoteUrl\":\"https://example.invalid/repo.git\","
      "\"intervalSeconds\":45,"
      "\"enabled\":true,"
      "\"autoCommitMerges\":true"
      "}";

  const nlohmann::json j = nlohmann::json::parse(legacy);

  // FromJson must not throw, must not crash, and must round-trip the other
  // fields unchanged.
  vxcore::SyncConfig cfg = vxcore::SyncConfig::FromJson(j);
  ASSERT_EQ(cfg.backend, std::string("git"));
  ASSERT_EQ(cfg.remote_url, std::string("https://example.invalid/repo.git"));
  ASSERT_EQ(cfg.interval_seconds, 45);
  ASSERT_TRUE(cfg.auto_commit_merges);

  // Round-trip back to JSON and confirm the legacy `enabled` key is gone.
  const nlohmann::json reserialized = cfg.ToJson();
  ASSERT_FALSE(reserialized.contains("enabled"));

  // Also exercise the `"enabled":false` case for completeness.
  const nlohmann::json j_false = nlohmann::json::parse(
      "{\"backend\":\"git\",\"remoteUrl\":\"x\",\"enabled\":false}");
  vxcore::SyncConfig cfg_false = vxcore::SyncConfig::FromJson(j_false);
  ASSERT_EQ(cfg_false.backend, std::string("git"));
  ASSERT_EQ(cfg_false.remote_url, std::string("x"));

  std::cout << "  PASSED test_enabled_field_ignored_in_deserialize" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);

  int failed = 0;
  RUN_TEST(test_enabled_field_absent_in_serialize);
  RUN_TEST(test_enabled_field_ignored_in_deserialize);

  if (failed > 0) {
    std::cerr << failed << " test(s) failed" << std::endl;
    return 1;
  }
  std::cout << "All tests passed" << std::endl;
  return 0;
}
