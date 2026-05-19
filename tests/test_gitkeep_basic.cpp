// T2 of the gitkeep-empty-folders plan. RED-state tests that drive the
// expected behavior of GitSyncBackend::EnsureGitkeepFiles (added in T5/T6/T7).
// Every test creates a temp notebook + bare remote, runs Sync, and asserts the
// resulting on-disk .gitkeep distribution. Because EnsureGitkeepFiles does not
// yet exist, every test in this file fails until the production code is in
// place. Mirrors the fixture pattern from test_git_sync_roundtrip.cpp.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "sync/git/git_sync_backend.h"
#include "sync/sync_types.h"
#include "test_git_sync_helpers.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore_types.h"

namespace {

// vxcore_set_test_mode lives in vxcore.dll which is NOT linked into this test
// (same compile pattern as test_git_sync_roundtrip: we only compile
// git_sync_backend.cpp + utils.cpp + the git_sync_test_helpers static lib).
// Our tests target get_test_path() which already resolves under %TEMP% rather
// than real AppData, so the production ConfigManager test-mode flag is moot.
// We provide a local no-op so that the mandated vxcore_set_test_mode(1) call
// in main() links cleanly. Defined in an anonymous namespace so it does not
// collide with the real C-linkage symbol if anyone ever does link vxcore.dll.
void vxcore_set_test_mode(int /*enabled*/) {}

// Lightweight fixture: temp paths + a GitSyncBackend in a known-initialized
// state. Each test instantiates one of these, populates folders, calls Sync,
// then asserts.
struct Fixture {
  std::string bare_path;
  std::string notebook_root;
  vxcore::GitSyncBackend backend;
};

// Sets up bare_path, notebook_root, seeds the bare repo with one initial
// commit, and Initializes the backend. Returns VXCORE_OK on success; the
// caller asserts. Mirrors the create_bare_repo + commit_file + Initialize
// trio used at the head of every test in test_git_sync_roundtrip.cpp.
VxCoreError setup_synced_notebook(Fixture &f, const std::string &test_tag) {
  f.bare_path = get_test_path(std::string("gitkeep_") + test_tag + "_bare");
  f.notebook_root = get_test_path(std::string("gitkeep_") + test_tag + "_nb");
  cleanup_test_dir(f.bare_path);
  cleanup_test_dir(f.notebook_root);

  const std::string bare_url = vxcore_test::create_bare_repo(f.bare_path);
  vxcore_test::commit_file(f.bare_path, "seed.md", "seed body", "init");

  vxcore::SyncConfig config;
  config.backend = "git";
  config.remote_url = bare_url;
  config.interval_seconds = 300;
  return f.backend.Initialize(f.notebook_root, config);
}

void teardown_fixture(Fixture &f) {
  cleanup_test_dir(f.bare_path);
  cleanup_test_dir(f.notebook_root);
}

bool keep_exists(const std::string &abs_dir) {
  return vxcore::PathExists(abs_dir + "/.gitkeep");
}

std::uintmax_t keep_size(const std::string &abs_dir) {
  std::error_code ec;
  return std::filesystem::file_size(
      vxcore::PathFromUtf8(abs_dir + "/.gitkeep"), ec);
}

// Recursively walks <root> and returns true iff any regular file named
// .gitkeep is found anywhere under it.
bool any_gitkeep_under(const std::string &root) {
  std::error_code ec;
  auto root_path = vxcore::PathFromUtf8(root);
  if (!std::filesystem::exists(root_path, ec)) return false;
  for (auto it = std::filesystem::recursive_directory_iterator(
           root_path, std::filesystem::directory_options::skip_permission_denied,
           ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
    if (it->is_regular_file(ec) && it->path().filename() == ".gitkeep") {
      return true;
    }
  }
  return false;
}

// 1. Empty user-visible folder gets a 0-byte .gitkeep after sync.
int test_empty_user_folder_gets_gitkeep() {
  std::cout << "  Running test_empty_user_folder_gets_gitkeep..." << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "empty"), VXCORE_OK);

    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/MyEmpty"));

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    ASSERT_TRUE(keep_exists(f.notebook_root + "/MyEmpty"));
    ASSERT_EQ(keep_size(f.notebook_root + "/MyEmpty"),
              static_cast<std::uintmax_t>(0));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_empty_user_folder_gets_gitkeep passed" << std::endl;
  return 0;
}

// 2. Nested empties: only the deepest empty leaf gets .gitkeep (Default #2:
//    folder A is not effectively empty when it contains a non-empty subtree).
int test_nested_empty_folders_each_get_gitkeep() {
  std::cout << "  Running test_nested_empty_folders_each_get_gitkeep..."
            << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "nested"), VXCORE_OK);

    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/A/B/C"));

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    // Deepest leaf gets the marker.
    ASSERT_TRUE(keep_exists(f.notebook_root + "/A/B/C"));
    ASSERT_EQ(keep_size(f.notebook_root + "/A/B/C"),
              static_cast<std::uintmax_t>(0));

    // Intermediate folders are NOT effectively empty (they hold a non-empty
    // child) so they MUST NOT get their own .gitkeep.
    ASSERT_FALSE(keep_exists(f.notebook_root + "/A"));
    ASSERT_FALSE(keep_exists(f.notebook_root + "/A/B"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_nested_empty_folders_each_get_gitkeep passed"
            << std::endl;
  return 0;
}

// 3. A user folder containing a real note must NOT receive a .gitkeep.
int test_folder_with_real_file_no_gitkeep() {
  std::cout << "  Running test_folder_with_real_file_no_gitkeep..."
            << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "withfile"), VXCORE_OK);

    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/Notes"));
    {
      std::ofstream ofs(vxcore::PathFromUtf8(f.notebook_root + "/Notes/note.md"),
                        std::ios::binary | std::ios::trunc);
      ofs << "real content";
    }

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    ASSERT_FALSE(keep_exists(f.notebook_root + "/Notes"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_folder_with_real_file_no_gitkeep passed" << std::endl;
  return 0;
}

// 4. The vx_notebook/ tree (which contains vx_sync/, our gitdir) is on the
//    skip-list. No .gitkeep may appear anywhere beneath it.
int test_skip_list_vx_notebook_never_touched() {
  std::cout << "  Running test_skip_list_vx_notebook_never_touched..."
            << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "vxnb"), VXCORE_OK);

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    ASSERT_FALSE(any_gitkeep_under(f.notebook_root + "/vx_notebook"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_skip_list_vx_notebook_never_touched passed" << std::endl;
  return 0;
}

// 5. vx_recycle_bin/ at the notebook root is skip-listed even when empty.
int test_skip_list_vx_recycle_bin_never_touched() {
  std::cout << "  Running test_skip_list_vx_recycle_bin_never_touched..."
            << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "recycle"), VXCORE_OK);

    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/vx_recycle_bin"));

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    ASSERT_FALSE(keep_exists(f.notebook_root + "/vx_recycle_bin"));
    ASSERT_FALSE(any_gitkeep_under(f.notebook_root + "/vx_recycle_bin"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_skip_list_vx_recycle_bin_never_touched passed"
            << std::endl;
  return 0;
}

// 6. Folders whose name begins with '.' are skip-listed at every depth.
int test_skip_list_dot_folder_never_touched() {
  std::cout << "  Running test_skip_list_dot_folder_never_touched..."
            << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "dot"), VXCORE_OK);

    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/.cache"));

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    ASSERT_FALSE(keep_exists(f.notebook_root + "/.cache"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_skip_list_dot_folder_never_touched passed" << std::endl;
  return 0;
}

// 7. Folders whose name begins with "_v_" are skip-listed at every depth.
int test_skip_list_underscore_v_folder_never_touched() {
  std::cout << "  Running test_skip_list_underscore_v_folder_never_touched..."
            << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "underv"), VXCORE_OK);

    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/_v_legacy"));

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    ASSERT_FALSE(keep_exists(f.notebook_root + "/_v_legacy"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout
      << "  test_skip_list_underscore_v_folder_never_touched passed"
      << std::endl;
  return 0;
}

// 8. The notebook root itself is never decorated with .gitkeep, even if it
//    happens to contain only skip-listed children.
int test_notebook_root_never_gets_gitkeep() {
  std::cout << "  Running test_notebook_root_never_gets_gitkeep..."
            << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "root"), VXCORE_OK);

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    ASSERT_FALSE(keep_exists(f.notebook_root));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_notebook_root_never_gets_gitkeep passed" << std::endl;
  return 0;
}

// 9. Idempotence: calling Sync twice on the same empty folder produces a
//    single 0-byte .gitkeep, unchanged across the second call.
int test_idempotent_on_repeated_sync() {
  std::cout << "  Running test_idempotent_on_repeated_sync..." << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "idem"), VXCORE_OK);

    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/Stays"));

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);
    ASSERT_TRUE(keep_exists(f.notebook_root + "/Stays"));
    auto first_write = std::filesystem::last_write_time(
        vxcore::PathFromUtf8(f.notebook_root + "/Stays/.gitkeep"));

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);
    ASSERT_TRUE(keep_exists(f.notebook_root + "/Stays"));
    ASSERT_EQ(keep_size(f.notebook_root + "/Stays"),
              static_cast<std::uintmax_t>(0));
    auto second_write = std::filesystem::last_write_time(
        vxcore::PathFromUtf8(f.notebook_root + "/Stays/.gitkeep"));
    ASSERT_TRUE(first_write == second_write);
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_idempotent_on_repeated_sync passed" << std::endl;
  return 0;
}

// 10. Nested folder happening to share a reserved root-level name is still a
//     user folder and DOES get .gitkeep (Default #3: skip-list is root-only).
int test_nested_reserved_name_still_gets_gitkeep() {
  std::cout << "  Running test_nested_reserved_name_still_gets_gitkeep..."
            << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "nestedres"), VXCORE_OK);

    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/MyNotes/vx_notebook"));

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    ASSERT_TRUE(keep_exists(f.notebook_root + "/MyNotes/vx_notebook"));
    ASSERT_EQ(keep_size(f.notebook_root + "/MyNotes/vx_notebook"),
              static_cast<std::uintmax_t>(0));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_nested_reserved_name_still_gets_gitkeep passed"
            << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  RUN_TEST(test_empty_user_folder_gets_gitkeep);
  RUN_TEST(test_nested_empty_folders_each_get_gitkeep);
  RUN_TEST(test_folder_with_real_file_no_gitkeep);
  RUN_TEST(test_skip_list_vx_notebook_never_touched);
  RUN_TEST(test_skip_list_vx_recycle_bin_never_touched);
  RUN_TEST(test_skip_list_dot_folder_never_touched);
  RUN_TEST(test_skip_list_underscore_v_folder_never_touched);
  RUN_TEST(test_notebook_root_never_gets_gitkeep);
  RUN_TEST(test_idempotent_on_repeated_sync);
  RUN_TEST(test_nested_reserved_name_still_gets_gitkeep);
  std::cout << "All test_gitkeep_basic tests passed" << std::endl;
  return 0;
}
