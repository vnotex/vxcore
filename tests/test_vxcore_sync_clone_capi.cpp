// T19 of open-notebook-remote-readonly: C-ABI tests for vxcore_sync_clone.
//
// Coverage (per plan T19 "What to do" subtests):
//   1. Happy path via C API: clone of file:// bare repo containing a VNote
//      bundled notebook -> VXCORE_OK; *out_notebook_id non-null; the cloned
//      notebook appears in vxcore_notebook_list. Asserts caller-frees-id
//      semantics via vxcore_string_free.
//   2. Malformed config_json -> VXCORE_ERR_JSON_PARSE and *out_notebook_id
//      stays nullptr (the C ABI clears the sentinel on entry).
//   3a. Missing "backend" in config_json -> VXCORE_ERR_INVALID_PARAM.
//   3b. Unknown backend name -> VXCORE_ERR_UNKNOWN_BACKEND.
//   4. credentials_json == "{}" (empty object) -> anonymous clone proceeds
//      using the InMemoryCredentialProvider-with-empty-PAT path; also
//      exercises the NULL credentials_json branch (NoOpCredentialProvider).
//      Both are valid against file:// fixtures and prove the symmetry with
//      vxcore_sync_enable's credentials handling.
//
// Test isolation: vxcore_set_test_mode(1) routes session data to
// %TEMP%\vxcore_test_config + %TEMP%\vxcore_test_data per AGENTS.md §
// "Test isolation on Windows". The bare-repo fixtures come from the
// git_sync_test_helpers static library shared with the other
// test_git_sync_*_capi.cpp binaries; see tests/CMakeLists.txt entry below.
//
// IMPLEMENTATION NOTE -- vxcore_sync_clone's impl uses nlohmann's no-throw
// parser for both JSON inputs (MSVC + nlohmann exception-unwind quirk wedges
// the test process when a throwing parse happens inside vxcore.dll across
// the C ABI boundary). The malformed-JSON subtest exercises that path.

#include <cstring>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "test_git_sync_helpers.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_types.h"

namespace {

// Minimal vx_notebook/config.json content: just enough fields for
// NotebookManager::OpenNotebook to accept the parsed config after the
// orchestrator's "id non-empty" gate. Mirrors the helper in
// test_sync_clone_orchestrator.cpp so the two suites stay in sync.
std::string MinimalNotebookConfigJson(const std::string &id, const std::string &name) {
  nlohmann::json cfg = nlohmann::json::object();
  cfg["id"] = id;
  cfg["name"] = name;
  cfg["description"] = "";
  cfg["assetsFolder"] = "vx_assets";
  cfg["metadata"] = nlohmann::json::object();
  cfg["tags"] = nlohmann::json::array();
  cfg["tagsModifiedUtc"] = 0;
  cfg["ignored"] = nlohmann::json::array();
  cfg["syncEnabled"] = false;
  cfg["syncBackend"] = "";
  cfg["syncRemoteUrl"] = "";
  cfg["autoSyncEnabled"] = true;
  return cfg.dump(2);
}

// Subtest 1 - Null-pointer hygiene. Required by the C ABI null-safety
// convention (every required out-pointer rejected with NULL_POINTER).
// Run first so a regression here surfaces immediately and doesn't waste
// later fixtures.
int test_clone_capi_null_pointer_args() {
  std::cout << "  Running test_clone_capi_null_pointer_args..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *out_id = nullptr;
  ASSERT_EQ(vxcore_sync_clone(nullptr, "/tmp/x", "{}", "{}", &out_id),
            VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_sync_clone(ctx, nullptr, "{}", "{}", &out_id), VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_sync_clone(ctx, "/tmp/x", nullptr, "{}", &out_id),
            VXCORE_ERR_NULL_POINTER);
  ASSERT_EQ(vxcore_sync_clone(ctx, "/tmp/x", "{}", "{}", nullptr),
            VXCORE_ERR_NULL_POINTER);

  vxcore_context_destroy(ctx);
  std::cout << "  test_clone_capi_null_pointer_args passed" << std::endl;
  return 0;
}

// Subtest 2 - Missing "backend" in config -> VXCORE_ERR_INVALID_PARAM.
// SyncConfig::FromJson treats absent "backend" as empty string, which the
// orchestrator rejects at the validation step.
int test_clone_capi_missing_backend() {
  std::cout << "  Running test_clone_capi_missing_backend..." << std::endl;

  const std::string target_dir = get_test_path("clone_capi_missing_backend_target");
  cleanup_test_dir(target_dir);
  create_directory(target_dir);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *out_id = nullptr;
  VxCoreError err = vxcore_sync_clone(
      ctx, target_dir.c_str(),
      /*config_json=*/"{\"remoteUrl\":\"file:///irrelevant\"}",
      /*credentials_json=*/"{}", &out_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  ASSERT_NULL(out_id);

  // Target dir must NOT have a vx_notebook/ subtree -- the orchestrator
  // rejected before invoking the backend.
  ASSERT_FALSE(path_exists(target_dir + "/vx_notebook"));

  vxcore_context_destroy(ctx);
  cleanup_test_dir(target_dir);
  std::cout << "  test_clone_capi_missing_backend passed" << std::endl;
  return 0;
}

// Subtest 3 - Unknown backend name -> VXCORE_ERR_UNKNOWN_BACKEND.
int test_clone_capi_unknown_backend() {
  std::cout << "  Running test_clone_capi_unknown_backend..." << std::endl;

  const std::string target_dir = get_test_path("clone_capi_unknown_backend_target");
  cleanup_test_dir(target_dir);
  create_directory(target_dir);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  char *out_id = nullptr;
  VxCoreError err = vxcore_sync_clone(
      ctx, target_dir.c_str(),
      /*config_json=*/
      "{\"backend\":\"nonexistent_backend_xyz\",\"remoteUrl\":\"file:///x\"}",
      /*credentials_json=*/"{}", &out_id);
  ASSERT_EQ(err, VXCORE_ERR_UNKNOWN_BACKEND);
  ASSERT_NULL(out_id);

  ASSERT_FALSE(path_exists(target_dir + "/vx_notebook"));

  vxcore_context_destroy(ctx);
  cleanup_test_dir(target_dir);
  std::cout << "  test_clone_capi_unknown_backend passed" << std::endl;
  return 0;
}

// Subtest 4 - Malformed config_json -> VXCORE_ERR_JSON_PARSE; out_id stays
// null after the C ABI's sentinel-clear. Also exercises malformed
// credentials_json (config valid) and the top-level-array shape that the
// no-throw parse would otherwise misinterpret as an empty config.
int test_clone_capi_malformed_config_json() {
  std::cout << "  Running test_clone_capi_malformed_config_json..." << std::endl;

  const std::string target_dir = get_test_path("clone_capi_malformed_target");
  cleanup_test_dir(target_dir);
  create_directory(target_dir);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  // Pre-seed out_id with a sentinel so we can prove the C ABI clears it on
  // failure (the documented "no notebook registered" contract -- see
  // vxcore.h comment for vxcore_sync_clone).
  char *sentinel = const_cast<char *>("sentinel-not-cleared");
  char *out_id = sentinel;
  VxCoreError err = vxcore_sync_clone(ctx, target_dir.c_str(),
                                       /*config_json=*/"not-json",
                                       /*credentials_json=*/"{}", &out_id);
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);
  ASSERT_NULL(out_id);

  // Top-level array. The no-throw parser succeeds but the impl rejects
  // is_object()==false explicitly so callers get a parse error instead of
  // a silent empty-config fallback.
  out_id = sentinel;
  err = vxcore_sync_clone(ctx, target_dir.c_str(), /*config_json=*/"[]",
                          /*credentials_json=*/"{}", &out_id);
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);
  ASSERT_NULL(out_id);

  // Malformed credentials_json must also surface as JSON_PARSE -- the config
  // half parses but the second no-throw call detects the bad creds payload.
  out_id = sentinel;
  err = vxcore_sync_clone(
      ctx, target_dir.c_str(),
      /*config_json=*/"{\"backend\":\"git\",\"remoteUrl\":\"file:///x\"}",
      /*credentials_json=*/"not-json", &out_id);
  ASSERT_EQ(err, VXCORE_ERR_JSON_PARSE);
  ASSERT_NULL(out_id);

  vxcore_context_destroy(ctx);
  cleanup_test_dir(target_dir);
  std::cout << "  test_clone_capi_malformed_config_json passed" << std::endl;
  return 0;
}

// Subtest 5 - credentials_json == "{}" (and NULL) must NOT fail: it installs
// an InMemoryCredentialProvider whose PAT is empty (or a
// NoOpCredentialProvider for NULL), which is the "anonymous clone" path that
// libgit2 takes against file:// fixtures. Mirrors the behaviour
// vxcore_sync_enable exposes for the same JSON shape.
int test_clone_capi_empty_credentials() {
  std::cout << "  Running test_clone_capi_empty_credentials..." << std::endl;

  const std::string bare_path = get_test_path("clone_capi_anon_bare");
  const std::string target_dir = get_test_path("clone_capi_anon_target");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(target_dir);

  const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
  const std::string expected_id = "clone-capi-anon-nb";
  vxcore_test::commit_file(bare_path, "vx_notebook/config.json",
                           MinimalNotebookConfigJson(expected_id, "Anon CAPI Clone"),
                           "seed: add vx_notebook/config.json");
  create_directory(target_dir);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  // First branch: explicit "{}". InMemoryCredentialProvider with empty PAT.
  const std::string config_json =
      std::string("{\"backend\":\"git\",\"remoteUrl\":\"") + bare_url + "\"}";
  char *out_id = nullptr;
  VxCoreError err = vxcore_sync_clone(ctx, target_dir.c_str(), config_json.c_str(),
                                       /*credentials_json=*/"{}", &out_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(out_id);
  ASSERT_EQ(std::string(out_id), expected_id);

  vxcore_string_free(out_id);
  out_id = nullptr;
  // Tear down between branches: close + recreate target_dir so the second
  // call starts from the same empty-tree precondition the API requires.
  ASSERT_EQ(vxcore_notebook_close(ctx, expected_id.c_str()), VXCORE_OK);
  cleanup_test_dir(target_dir);
  create_directory(target_dir);

  // Second branch: NULL credentials_json -> NoOpCredentialProvider.
  err = vxcore_sync_clone(ctx, target_dir.c_str(), config_json.c_str(),
                          /*credentials_json=*/nullptr, &out_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(out_id);
  ASSERT_EQ(std::string(out_id), expected_id);

  vxcore_string_free(out_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(bare_path);
  cleanup_test_dir(target_dir);
  std::cout << "  test_clone_capi_empty_credentials passed" << std::endl;
  return 0;
}

// Subtest 6 - Happy path through the C ABI: clone + register + list.
// Last so all upstream validation/null/parse subtests have exercised the
// fast paths and any libgit2 init bug surfaces here as a focused failure.
int test_clone_capi_happy_path() {
  std::cout << "  Running test_clone_capi_happy_path..." << std::endl;

  const std::string bare_path = get_test_path("clone_capi_happy_bare");
  const std::string target_dir = get_test_path("clone_capi_happy_target");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(target_dir);

  // Seed the bare remote with a minimal VNote bundled notebook.
  const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
  const std::string expected_id = "clone-capi-happy-nb";
  vxcore_test::commit_file(bare_path, "vx_notebook/config.json",
                           MinimalNotebookConfigJson(expected_id, "Happy CAPI Clone"),
                           "seed: add vx_notebook/config.json");

  // Contract: caller pre-creates target_dir.
  create_directory(target_dir);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  ASSERT_NOT_NULL(ctx);

  // Build the same config shape vxcore_sync_enable accepts, so the JSON
  // helper that produces enable_cfg can be reused for clone_cfg.
  const std::string config_json =
      std::string("{\"backend\":\"git\",\"remoteUrl\":\"") + bare_url +
      "\",\"autoSyncEnabled\":true}";
  // Anonymous clone: empty creds object. vxcore_sync_clone treats this as
  // "InMemoryCredentialProvider with empty PAT", which libgit2 resolves
  // anonymously against file:// remotes.
  const char *creds_json = "{}";

  char *out_id = nullptr;
  VxCoreError err = vxcore_sync_clone(ctx, target_dir.c_str(), config_json.c_str(),
                                       creds_json, &out_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(out_id);
  std::string out_id_str(out_id);
  ASSERT_EQ(out_id_str, expected_id);

  // Working tree + git layout end up where BootstrapFromEmptyRemote puts
  // them. These are the same anchor files test_clone_orchestrator_happy_path
  // checks, repeated here so a regression in either layer fails this test.
  ASSERT_TRUE(path_exists(target_dir + "/vx_notebook/config.json"));
  ASSERT_TRUE(path_exists(target_dir + "/vx_notebook/vx_sync/HEAD"));

  // Registration must round-trip through the public listing path. We don't
  // require a particular JSON shape, just that the new id is reachable.
  char *list_json = nullptr;
  ASSERT_EQ(vxcore_notebook_list(ctx, &list_json), VXCORE_OK);
  ASSERT_NOT_NULL(list_json);
  std::string list_str(list_json);
  vxcore_string_free(list_json);
  ASSERT_TRUE(list_str.find(expected_id) != std::string::npos);

  // Caller-frees-out contract.
  vxcore_string_free(out_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(bare_path);
  cleanup_test_dir(target_dir);
  std::cout << "  test_clone_capi_happy_path passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running vxcore_sync_clone C ABI tests..." << std::endl;
  RUN_TEST(test_clone_capi_null_pointer_args);
  RUN_TEST(test_clone_capi_missing_backend);
  RUN_TEST(test_clone_capi_unknown_backend);
  RUN_TEST(test_clone_capi_malformed_config_json);
  RUN_TEST(test_clone_capi_empty_credentials);
  RUN_TEST(test_clone_capi_happy_path);

  std::cout << "All vxcore_sync_clone C ABI tests passed" << std::endl;
  return 0;
}
