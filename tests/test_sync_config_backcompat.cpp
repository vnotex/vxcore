// Clean-break behavior for SyncConfig's sync gate (sync-interval-debounce).
//
// The legacy integer interval gate was removed and replaced by a single
// boolean gate `auto_sync_enabled` (JSON key `"autoSyncEnabled"`, default
// true). This is a CLEAN BREAK: there is no migration path that reads the old
// integer keys, so a config that lacks `"autoSyncEnabled"` simply falls back
// to the default (true) — exactly as if the legacy keys were never there.
//
// This test pins the new contracts:
//   - Serializer emits `"autoSyncEnabled"` and NEVER the long-removed
//     `"enabled"` flag.
//   - Deserializer defaults `auto_sync_enabled` to true when `"autoSyncEnabled"`
//     is absent (the legacy on-disk shape), honors an explicit boolean, and
//     silently ignores the removed `"enabled"` key.
//
// It links only against `vxcore`'s public API surface for
// `SyncConfig::FromJson`/`ToJson` — no backend wiring required.

#include <iostream>
#include <nlohmann/json.hpp>

#include "sync/sync_types.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace {

int test_serialize_emits_bool_gate() {
  std::cout << "  Running test_serialize_emits_bool_gate..." << std::endl;

  vxcore::SyncConfig cfg;
  cfg.backend = "git";
  cfg.remote_url = "https://example.invalid/repo.git";
  cfg.auto_sync_enabled = false;  // non-default, so the round-trip is meaningful
  cfg.exclude_paths = {"*.tmp"};
  cfg.auto_commit_merges = false;

  const nlohmann::json j = cfg.ToJson();

  // The boolean gate is emitted and round-trips the value we set.
  ASSERT_TRUE(j.contains("autoSyncEnabled"));
  ASSERT_FALSE(j["autoSyncEnabled"].get<bool>());

  // The other fields are still emitted as before.
  ASSERT_TRUE(j.contains("backend"));
  ASSERT_TRUE(j.contains("remoteUrl"));
  ASSERT_TRUE(j.contains("excludePaths"));
  ASSERT_TRUE(j.contains("autoCommitMerges"));

  // The long-removed `enabled` flag must NEVER appear, including via a sibling
  // or nested key.
  ASSERT_FALSE(j.contains("enabled"));
  const std::string dumped = j.dump();
  ASSERT_TRUE(dumped.find("\"enabled\"") == std::string::npos);

  std::cout << "  PASSED test_serialize_emits_bool_gate" << std::endl;
  return 0;
}

int test_deserialize_defaults_when_gate_absent() {
  std::cout << "  Running test_deserialize_defaults_when_gate_absent..." << std::endl;

  // Legacy on-disk shape: no `autoSyncEnabled` key at all (older builds never
  // wrote it). FromJson must fall back to the default of true. The removed
  // `enabled` flag, if present, is silently dropped.
  const std::string legacy =
      "{"
      "\"backend\":\"git\","
      "\"remoteUrl\":\"https://example.invalid/repo.git\","
      "\"enabled\":true,"
      "\"autoCommitMerges\":true"
      "}";

  const nlohmann::json j = nlohmann::json::parse(legacy);
  vxcore::SyncConfig cfg = vxcore::SyncConfig::FromJson(j);
  ASSERT_EQ(cfg.backend, std::string("git"));
  ASSERT_EQ(cfg.remote_url, std::string("https://example.invalid/repo.git"));
  ASSERT_TRUE(cfg.auto_sync_enabled);  // absent gate => default true
  ASSERT_TRUE(cfg.auto_commit_merges);

  // Round-trip back to JSON: the boolean gate is present (defaulted to true)
  // and the legacy `enabled` key is gone.
  const nlohmann::json reserialized = cfg.ToJson();
  ASSERT_TRUE(reserialized.contains("autoSyncEnabled"));
  ASSERT_TRUE(reserialized["autoSyncEnabled"].get<bool>());
  ASSERT_FALSE(reserialized.contains("enabled"));

  // Explicit `"autoSyncEnabled":false` is honored even when the removed
  // `enabled` key is also present.
  const nlohmann::json j_false = nlohmann::json::parse(
      "{\"backend\":\"git\",\"remoteUrl\":\"x\",\"autoSyncEnabled\":false,\"enabled\":true}");
  vxcore::SyncConfig cfg_false = vxcore::SyncConfig::FromJson(j_false);
  ASSERT_EQ(cfg_false.backend, std::string("git"));
  ASSERT_EQ(cfg_false.remote_url, std::string("x"));
  ASSERT_FALSE(cfg_false.auto_sync_enabled);

  std::cout << "  PASSED test_deserialize_defaults_when_gate_absent" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);

  int failed = 0;
  RUN_TEST(test_serialize_emits_bool_gate);
  RUN_TEST(test_deserialize_defaults_when_gate_absent);

  if (failed > 0) {
    std::cerr << failed << " test(s) failed" << std::endl;
    return 1;
  }
  std::cout << "All tests passed" << std::endl;
  return 0;
}
