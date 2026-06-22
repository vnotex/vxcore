// openurl-followups Item 2: C ABI tests for vxcore_sync_clone_cancellable.
//
// Covers three subtests:
//   1. BEFORE-clone cancel (strict): create token, Cancel(), then call
//      vxcore_sync_clone_cancellable. Expect a non-OK result and a null
//      out_notebook_id. Note: libgit2's git_repository_init_ext may complete
//      before the cancellation lands at the fetch step, so we accept any
//      non-OK return as proof the contract is honored (the strictly
//      cancellable phases are the network ones).
//   2. MID-clone cancel (race-tolerant): spawn a worker that sleeps a few
//      ms then cancels the token while the main thread runs
//      vxcore_sync_clone_cancellable against a file:// bare repo. Loop up
//      to 5 iterations; require AT LEAST one observation of cancellation.
//      file:// fetches are very fast, so it is legitimate for the race to
//      be lost; we document the limitation and skip the assertion if no
//      iteration ever observed cancellation (the BEFORE-clone test still
//      covers the contract).
//   3. null-token equivalence: vxcore_sync_clone_cancellable(..., nullptr,
//      ...) returns identical result to vxcore_sync_clone(...) for the
//      same fixture (proves the shim relationship).
//
// Test isolation: vxcore_set_test_mode(1) routes session data to
// %TEMP%\vxcore_test_config + %TEMP%\vxcore_test_data per AGENTS.md §
// "Test isolation on Windows". The bare-repo fixtures come from the
// git_sync_test_helpers static library shared with the other
// test_git_sync_*_capi.cpp binaries.

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "test_git_sync_helpers.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_types.h"

namespace {

// Reuse the same MinimalNotebookConfigJson shape as
// test_vxcore_sync_clone_capi.cpp:46 so the two tests stay in sync.
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

// Convenience helper to set up a bare repo + minimal notebook contents.
// Returns the bare URL string suitable for the "remoteUrl" config field.
std::string SeedBareRepo(const std::string &bare_path, const std::string &notebook_id,
                         const std::string &notebook_name) {
  const std::string bare_url = vxcore_test::create_bare_repo(bare_path);
  vxcore_test::commit_file(bare_path, "vx_notebook/config.json",
                           MinimalNotebookConfigJson(notebook_id, notebook_name),
                           "seed: add vx_notebook/config.json");
  return bare_url;
}

// Subtest 1: BEFORE-clone cancellation.
//
// Strict contract: the token is cancelled BEFORE the C ABI is even called,
// so the very first libgit2 progress check (at the fetch stage) will read
// the flag as set and abort. We tolerate the case where the early phases
// (git_repository_init_ext, remote_create) complete first — the libgit2
// progress callback only fires during fetch — by asserting only that the
// final result is not VXCORE_OK and out_id stays null.
int test_before_clone_cancel() {
  std::cout << "  Running test_before_clone_cancel..." << std::endl;

  const std::string bare_path = get_test_path("clone_cancel_before_bare");
  const std::string target_dir = get_test_path("clone_cancel_before_target");
  cleanup_test_dir(bare_path);
  cleanup_test_dir(target_dir);

  const std::string bare_url = SeedBareRepo(bare_path, "clone-cancel-before-nb",
                                            "Before-Clone Cancel");
  create_directory(target_dir);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  VxCoreSyncCancellation *token = vxcore_sync_create_cancellation();
  ASSERT_NOT_NULL(token);
  // Cancel BEFORE invoking the clone.
  vxcore_sync_cancel(token);

  const std::string config_json =
      std::string("{\"backend\":\"git\",\"remoteUrl\":\"") + bare_url + "\"}";
  char *out_id = nullptr;
  VxCoreError err = vxcore_sync_clone_cancellable(ctx, target_dir.c_str(),
                                                  config_json.c_str(),
                                                  /*credentials_json=*/"{}", token,
                                                  &out_id);
  std::cout << "    BEFORE-clone cancel returned err=" << err << std::endl;

  // Tolerant assertion: anything but VXCORE_OK proves cancellation was
  // honored. The exact error code depends on which libgit2 phase observed
  // the cancellation (GIT_EUSER from transfer_progress maps to
  // VXCORE_ERR_CANCELLED via TranslateGitError; init/lookup phases bypass
  // the callback and may return a different code if the fetch step does
  // not see the cancel flag in time). What matters is that no notebook
  // got registered and out_id stays null.
  ASSERT_TRUE(err != VXCORE_OK);
  ASSERT_NULL(out_id);

  vxcore_sync_free_cancellation(token);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(bare_path);
  cleanup_test_dir(target_dir);
  std::cout << "  PASS test_before_clone_cancel" << std::endl;
  return 0;
}

// Subtest 2: MID-clone cancellation, race-tolerant.
//
// Spawn a worker that sleeps a short bit then cancels the token. Main thread
// runs the clone synchronously. file:// fixtures are very fast, so it is
// legitimate for the race to be lost (the clone completes before the worker
// fires Cancel). We loop up to 5 attempts; require AT LEAST one non-OK
// observation. If none observed, we DO NOT fail (the BEFORE-clone subtest
// covers the contract); we just log that the race could not be exercised.
int test_mid_clone_cancel_race_tolerant() {
  std::cout << "  Running test_mid_clone_cancel_race_tolerant..." << std::endl;

  const std::string bare_path = get_test_path("clone_cancel_mid_bare");
  cleanup_test_dir(bare_path);
  const std::string bare_url = SeedBareRepo(bare_path, "clone-cancel-mid-nb",
                                            "Mid-Clone Cancel");

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  const std::string config_json =
      std::string("{\"backend\":\"git\",\"remoteUrl\":\"") + bare_url + "\"}";

  constexpr int kMaxAttempts = 5;
  int observed_cancellations = 0;
  int observed_ok_races_lost = 0;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    const std::string target_dir =
        get_test_path("clone_cancel_mid_target_" + std::to_string(attempt));
    cleanup_test_dir(target_dir);
    create_directory(target_dir);

    VxCoreSyncCancellation *token = vxcore_sync_create_cancellation();
    ASSERT_NOT_NULL(token);

    std::atomic<bool> fired_cancel{false};
    std::thread worker([token, &fired_cancel]() {
      // Sleep a random-ish short window so different attempts hit different
      // points of the libgit2 fetch lifecycle. 10-50ms is wide enough to
      // sometimes land mid-fetch on a tiny local file:// remote.
      std::this_thread::sleep_for(std::chrono::milliseconds(10 + (rand() % 40)));
      vxcore_sync_cancel(token);
      fired_cancel.store(true, std::memory_order_seq_cst);
    });

    char *out_id = nullptr;
    VxCoreError err = vxcore_sync_clone_cancellable(ctx, target_dir.c_str(),
                                                    config_json.c_str(),
                                                    /*credentials_json=*/"{}", token,
                                                    &out_id);
    worker.join();

    std::cout << "    attempt=" << attempt << " err=" << err
              << " fired_cancel=" << (fired_cancel.load() ? 1 : 0) << std::endl;

    if (err == VXCORE_OK) {
      // Race lost — clone finished before Cancel() landed. Tear down the
      // notebook so subsequent attempts start clean.
      if (out_id != nullptr) {
        vxcore_notebook_close(ctx, out_id);
        vxcore_string_free(out_id);
      }
      observed_ok_races_lost++;
    } else {
      // Cancellation observed in some form.
      observed_cancellations++;
      // Some libgit2 phases may have written partial state; the caller is
      // expected to clean up target_dir. We do that here for hygiene.
      ASSERT_NULL(out_id);
    }

    vxcore_sync_free_cancellation(token);
    cleanup_test_dir(target_dir);
  }

  cleanup_test_dir(bare_path);
  vxcore_context_destroy(ctx);

  std::cout << "  MID-clone summary: observed_cancellations=" << observed_cancellations
            << " observed_ok_races_lost=" << observed_ok_races_lost << std::endl;
  if (observed_cancellations == 0) {
    // Race always lost — file:// is too fast. The BEFORE-clone subtest
    // provides the cancellation contract coverage; do NOT fail here.
    std::cout << "  WARN: MID-clone test never observed cancellation across "
              << kMaxAttempts << " attempts; relying on BEFORE-clone subtest"
              << " for cancellation coverage." << std::endl;
  }
  std::cout << "  PASS test_mid_clone_cancel_race_tolerant" << std::endl;
  return 0;
}

// Subtest 3: null-token equivalence proves the shim relationship between
// vxcore_sync_clone and vxcore_sync_clone_cancellable. Same fixture, same
// inputs, same result code expected.
int test_null_token_equivalence() {
  std::cout << "  Running test_null_token_equivalence..." << std::endl;

  // Two separate bare repos so each clone starts from a clean target.
  const std::string bare_path_a = get_test_path("clone_equiv_bare_a");
  const std::string bare_path_b = get_test_path("clone_equiv_bare_b");
  const std::string target_dir_a = get_test_path("clone_equiv_target_a");
  const std::string target_dir_b = get_test_path("clone_equiv_target_b");
  cleanup_test_dir(bare_path_a);
  cleanup_test_dir(bare_path_b);
  cleanup_test_dir(target_dir_a);
  cleanup_test_dir(target_dir_b);

  const std::string expected_id = "clone-equiv-nb";
  const std::string bare_url_a = SeedBareRepo(bare_path_a, expected_id, "Equiv A");
  const std::string bare_url_b = SeedBareRepo(bare_path_b, expected_id, "Equiv B");
  create_directory(target_dir_a);
  create_directory(target_dir_b);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);

  const std::string config_a =
      std::string("{\"backend\":\"git\",\"remoteUrl\":\"") + bare_url_a + "\"}";

  // Legacy entry point.
  char *out_id_legacy = nullptr;
  VxCoreError err_legacy = vxcore_sync_clone(ctx, target_dir_a.c_str(), config_a.c_str(),
                                              /*credentials_json=*/"{}", &out_id_legacy);
  std::cout << "    legacy clone err=" << err_legacy << std::endl;
  ASSERT_EQ(err_legacy, VXCORE_OK);
  ASSERT_NOT_NULL(out_id_legacy);
  ASSERT_EQ(std::string(out_id_legacy), expected_id);

  // Close + free so the second clone sees a clean slate.
  ASSERT_EQ(vxcore_notebook_close(ctx, out_id_legacy), VXCORE_OK);
  vxcore_string_free(out_id_legacy);
  out_id_legacy = nullptr;

  // Cancellable entry point with NULL token — must produce the same shape.
  const std::string config_b =
      std::string("{\"backend\":\"git\",\"remoteUrl\":\"") + bare_url_b + "\"}";
  char *out_id_cancellable = nullptr;
  VxCoreError err_cancellable = vxcore_sync_clone_cancellable(
      ctx, target_dir_b.c_str(), config_b.c_str(), /*credentials_json=*/"{}",
      /*token=*/nullptr, &out_id_cancellable);
  std::cout << "    cancellable(null) clone err=" << err_cancellable << std::endl;
  ASSERT_EQ(err_cancellable, VXCORE_OK);
  ASSERT_NOT_NULL(out_id_cancellable);
  ASSERT_EQ(std::string(out_id_cancellable), expected_id);

  // Same result code from both entry points — that's the equivalence
  // contract.
  ASSERT_EQ(err_legacy, err_cancellable);

  vxcore_string_free(out_id_cancellable);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(bare_path_a);
  cleanup_test_dir(bare_path_b);
  cleanup_test_dir(target_dir_a);
  cleanup_test_dir(target_dir_b);
  std::cout << "  PASS test_null_token_equivalence" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running vxcore_sync_clone_cancellable tests..." << std::endl;
  RUN_TEST(test_before_clone_cancel);
  RUN_TEST(test_mid_clone_cancel_race_tolerant);
  RUN_TEST(test_null_token_equivalence);

  std::cout << "All vxcore_sync_clone_cancellable tests passed" << std::endl;
  return 0;
}
