// T18 of open-notebook-remote-readonly plan: tests for the backend-agnostic
// SyncManager::CloneNotebook orchestrator that lives in libs/vxcore/src/sync/
// sync_manager.cpp.
//
// Coverage (per plan T18 "What to do" subtests):
//   1. Happy path: full clone of file:// bare repo containing a VNote bundled
//      notebook → VXCORE_OK; out_notebook_id non-empty + matches the id from
//      vx_notebook/config.json; cloned notebook appears in vxcore_notebook_list.
//   2. Validation: clone of git repo WITHOUT vx_notebook/config.json →
//      VXCORE_ERR_NOT_FOUND; no notebook registered.
//   3. Validation: clone of git repo with vx_notebook/config.json that has
//      empty "id" → VXCORE_ERR_INVALID_STATE; no notebook registered.
//   4. Routing: unknown backend name → VXCORE_ERR_UNKNOWN_BACKEND; nothing
//      touched on disk.
//
// Bare-repo fixture pattern is shared with test_git_sync_clone (T13). This
// test exercises the C++-only SyncManager::CloneNotebook overload directly
// via the VxCoreContext sync_manager pointer (vxcore_sync_clone C ABI is T19,
// not yet committed).
//
// The test target is registered via add_vxcore_test() (links vxcore.dll) plus
// an extra target_link_libraries(git_sync_test_helpers) for the bare-repo
// fixture; same pattern as test_sync_credentials_api.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "core/context.h"
#include "core/notebook_manager.h"
#include "sync/credential_provider.h"
#include "sync/sync_manager.h"
#include "sync/sync_types.h"
#include "test_git_sync_helpers.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_types.h"

using namespace vxcore;

namespace {

vxcore::VxCoreContext *as_ctx(VxCoreContextHandle h) {
  return reinterpret_cast<vxcore::VxCoreContext *>(h);
}

// Build the minimal vx_notebook/config.json content the orchestrator needs:
// just a non-empty "id" so the post-clone validation passes. Other fields
// follow NotebookConfig::ToJson defaults so NotebookManager::OpenNotebook is
// happy when it parses the cloned file.
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
  cfg["syncIntervalSeconds"] = 60;
  return cfg.dump(2);
}

// Subtest 1 — Happy path: VXCORE_OK + registration.
int test_clone_orchestrator_happy_path() {
  std::cout << "  Running test_clone_orchestrator_happy_path..." << std::endl;

  const std::string bare_path = get_test_path("clone_orch_happy_bare");
  const std::string target_dir = get_test_path("clone_orch_happy_target");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(target_dir);

  // Seed the bare repo with a valid vx_notebook/config.json.
  const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
  const std::string expected_id = "clone-orch-happy-nb";
  vxcore_test::commit_file(bare_path, "vx_notebook/config.json",
                           MinimalNotebookConfigJson(expected_id, "Happy Clone"),
                           "seed: add vx_notebook/config.json");

  // Caller contract: target_dir exists and is empty.
  create_directory(target_dir);

  VxCoreContextHandle handle = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &handle), VXCORE_OK);
  ASSERT_NOT_NULL(handle);
  vxcore::VxCoreContext *ctx = as_ctx(handle);
  ASSERT_NOT_NULL(ctx);
  ASSERT_NOT_NULL(ctx->sync_manager.get());

  SyncConfig config;
  config.backend = "git";
  config.remote_url = bare_url;
  config.interval_seconds = 300;

  std::string out_id;
  VxCoreError err =
      ctx->sync_manager->CloneNotebook(target_dir, config, /*provider=*/nullptr, out_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(out_id, expected_id);

  // Cloned working tree must contain the config file that drove validation.
  ASSERT_TRUE(path_exists(target_dir + "/vx_notebook/config.json"));
  // .git layout must live at vx_notebook/vx_sync/ per BootstrapFromEmptyRemote.
  ASSERT_TRUE(path_exists(target_dir + "/vx_notebook/vx_sync/HEAD"));

  // Registration must round-trip through vxcore_notebook_list.
  char *list_json = nullptr;
  ASSERT_EQ(vxcore_notebook_list(handle, &list_json), VXCORE_OK);
  ASSERT_NOT_NULL(list_json);
  std::string list_str(list_json);
  vxcore_string_free(list_json);
  ASSERT_TRUE(list_str.find(expected_id) != std::string::npos);

  vxcore_context_destroy(handle);
  cleanup_test_dir(bare_path);
  cleanup_test_dir(target_dir);
  std::cout << "  test_clone_orchestrator_happy_path passed" << std::endl;
  return 0;
}

// Subtest 2 — Missing config.json: VXCORE_ERR_NOT_FOUND, no registration.
int test_clone_orchestrator_no_config_json() {
  std::cout << "  Running test_clone_orchestrator_no_config_json..." << std::endl;

  const std::string bare_path = get_test_path("clone_orch_noconfig_bare");
  const std::string target_dir = get_test_path("clone_orch_noconfig_target");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(target_dir);

  // Seed bare repo with a plain README — no vx_notebook/ subtree at all.
  const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
  vxcore_test::commit_file(bare_path, "README.md", "Not a vnote notebook\n",
                           "seed: plain repo");

  create_directory(target_dir);

  VxCoreContextHandle handle = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &handle), VXCORE_OK);
  vxcore::VxCoreContext *ctx = as_ctx(handle);

  SyncConfig config;
  config.backend = "git";
  config.remote_url = bare_url;
  config.interval_seconds = 300;

  std::string out_id;
  VxCoreError err =
      ctx->sync_manager->CloneNotebook(target_dir, config, /*provider=*/nullptr, out_id);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT_TRUE(out_id.empty());

  // Clone DID populate the working tree (libgit2 succeeded). The orchestrator
  // contract is "caller cleans up target_dir on failure" — so the dir may
  // contain README.md + vx_notebook/vx_sync/.  What MUST NOT exist is a
  // VNote config file, and no notebook should appear in vxcore_notebook_list.
  ASSERT_FALSE(path_exists(target_dir + "/vx_notebook/config.json"));

  char *list_json = nullptr;
  ASSERT_EQ(vxcore_notebook_list(handle, &list_json), VXCORE_OK);
  ASSERT_NOT_NULL(list_json);
  std::string list_str(list_json);
  vxcore_string_free(list_json);
  // Stronger: target_dir should not appear in the list as a rootFolder.
  // Normalize backslashes so the path comparison is stable across platforms.
  std::string normalized_target = normalize_path(target_dir);
  std::string normalized_list = list_str;
  std::replace(normalized_list.begin(), normalized_list.end(), '\\', '/');
  ASSERT_TRUE(normalized_list.find(normalized_target) == std::string::npos);

  vxcore_context_destroy(handle);
  cleanup_test_dir(bare_path);
  cleanup_test_dir(target_dir);
  std::cout << "  test_clone_orchestrator_no_config_json passed" << std::endl;
  return 0;
}

// Subtest 3 — Empty "id" in config.json: VXCORE_ERR_INVALID_STATE.
int test_clone_orchestrator_empty_id() {
  std::cout << "  Running test_clone_orchestrator_empty_id..." << std::endl;

  const std::string bare_path = get_test_path("clone_orch_emptyid_bare");
  const std::string target_dir = get_test_path("clone_orch_emptyid_target");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(target_dir);

  const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
  // Valid JSON, but id is an empty string — orchestrator must reject.
  vxcore_test::commit_file(bare_path, "vx_notebook/config.json",
                           MinimalNotebookConfigJson("", "Empty Id"),
                           "seed: empty id");

  create_directory(target_dir);

  VxCoreContextHandle handle = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &handle), VXCORE_OK);
  vxcore::VxCoreContext *ctx = as_ctx(handle);

  SyncConfig config;
  config.backend = "git";
  config.remote_url = bare_url;
  config.interval_seconds = 300;

  std::string out_id;
  VxCoreError err =
      ctx->sync_manager->CloneNotebook(target_dir, config, /*provider=*/nullptr, out_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_STATE);
  ASSERT_TRUE(out_id.empty());

  char *list_json = nullptr;
  ASSERT_EQ(vxcore_notebook_list(handle, &list_json), VXCORE_OK);
  ASSERT_NOT_NULL(list_json);
  std::string list_str(list_json);
  vxcore_string_free(list_json);
  std::string normalized_target = normalize_path(target_dir);
  std::string normalized_list = list_str;
  std::replace(normalized_list.begin(), normalized_list.end(), '\\', '/');
  ASSERT_TRUE(normalized_list.find(normalized_target) == std::string::npos);

  vxcore_context_destroy(handle);
  cleanup_test_dir(bare_path);
  cleanup_test_dir(target_dir);
  std::cout << "  test_clone_orchestrator_empty_id passed" << std::endl;
  return 0;
}

// Subtest 4 — Unknown backend name: VXCORE_ERR_UNKNOWN_BACKEND, no FS touch.
int test_clone_orchestrator_unknown_backend() {
  std::cout << "  Running test_clone_orchestrator_unknown_backend..." << std::endl;

  const std::string target_dir = get_test_path("clone_orch_unknown_target");
  cleanup_test_dir(target_dir);
  create_directory(target_dir);

  VxCoreContextHandle handle = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &handle), VXCORE_OK);
  vxcore::VxCoreContext *ctx = as_ctx(handle);

  SyncConfig config;
  config.backend = "nonexistent_backend_xyz";
  config.remote_url = "file:///does/not/matter";
  config.interval_seconds = 300;

  std::string out_id;
  VxCoreError err =
      ctx->sync_manager->CloneNotebook(target_dir, config, /*provider=*/nullptr, out_id);
  ASSERT_EQ(err, VXCORE_ERR_UNKNOWN_BACKEND);
  ASSERT_TRUE(out_id.empty());

  // Orchestrator must NOT have touched the target dir.
  ASSERT_FALSE(path_exists(target_dir + "/vx_notebook"));
  ASSERT_FALSE(path_exists(target_dir + "/vx_notebook/vx_sync"));

  vxcore_context_destroy(handle);
  cleanup_test_dir(target_dir);
  std::cout << "  test_clone_orchestrator_unknown_backend passed" << std::endl;
  return 0;
}

// Subtest 5 — Empty backend in config: VXCORE_ERR_INVALID_PARAM.
int test_clone_orchestrator_empty_backend() {
  std::cout << "  Running test_clone_orchestrator_empty_backend..." << std::endl;

  const std::string target_dir = get_test_path("clone_orch_empty_backend_target");
  cleanup_test_dir(target_dir);
  create_directory(target_dir);

  VxCoreContextHandle handle = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &handle), VXCORE_OK);
  vxcore::VxCoreContext *ctx = as_ctx(handle);

  SyncConfig config;
  config.backend = "";  // The validation trigger.
  config.remote_url = "file:///irrelevant";
  config.interval_seconds = 300;

  std::string out_id;
  VxCoreError err =
      ctx->sync_manager->CloneNotebook(target_dir, config, /*provider=*/nullptr, out_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  ASSERT_TRUE(out_id.empty());

  ASSERT_FALSE(path_exists(target_dir + "/vx_notebook"));

  vxcore_context_destroy(handle);
  cleanup_test_dir(target_dir);
  std::cout << "  test_clone_orchestrator_empty_backend passed" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running SyncManager::CloneNotebook orchestrator tests..." << std::endl;
  RUN_TEST(test_clone_orchestrator_empty_backend);
  RUN_TEST(test_clone_orchestrator_unknown_backend);
  RUN_TEST(test_clone_orchestrator_no_config_json);
  RUN_TEST(test_clone_orchestrator_empty_id);
  RUN_TEST(test_clone_orchestrator_happy_path);

  std::cout << "All SyncManager::CloneNotebook orchestrator tests passed" << std::endl;
  return 0;
}
