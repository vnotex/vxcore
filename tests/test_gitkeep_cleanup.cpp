// T3 of the gitkeep-empty-folders plan. RED-state tests that drive the
// cleanup + marker-ownership semantics of GitSyncBackend::EnsureGitkeepFiles
// (added in T5/T6/T7). Every test creates a temp notebook + bare remote, runs
// Sync, and asserts the resulting on-disk .gitkeep state. Because
// EnsureGitkeepFiles does not yet exist, every test in this file fails until
// the production code is in place. Mirrors the fixture pattern from T2
// (test_gitkeep_basic.cpp) — the setup helper is intentionally duplicated
// here rather than extracted into a shared header.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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
// (same compile pattern as test_git_sync_roundtrip / test_gitkeep_basic: we
// only compile git_sync_backend.cpp + utils.cpp + the git_sync_test_helpers
// static lib). Our tests target get_test_path() which already resolves under
// %TEMP% rather than real AppData, so the production ConfigManager test-mode
// flag is moot. We provide a local no-op so that the mandated
// vxcore_set_test_mode(1) call in main() links cleanly. Defined in an
// anonymous namespace so it does not collide with the real C-linkage symbol
// if anyone ever does link vxcore.dll.
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
  f.bare_path = get_test_path(std::string("gitkeep_cu_") + test_tag + "_bare");
  f.notebook_root =
      get_test_path(std::string("gitkeep_cu_") + test_tag + "_nb");
  cleanup_test_dir(f.bare_path);
  cleanup_test_dir(f.notebook_root);

  const std::string bare_url = vxcore_test::create_bare_repo(f.bare_path);
  vxcore_test::commit_file(f.bare_path, "seed.md", "seed body", "init");

  vxcore::SyncConfig config;
  config.backend = "git";
  config.remote_url = bare_url;
  config.auto_sync_enabled = true;
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

bool file_exists(const std::string &abs_path) {
  return vxcore::PathExists(abs_path);
}

// Write <content> verbatim to <abs_path> in binary mode (no CRLF translation).
// Parent directory must already exist.
void write_file(const std::string &abs_path, const std::string &content) {
  std::ofstream ofs(vxcore::PathFromUtf8(abs_path),
                    std::ios::binary | std::ios::trunc);
  if (!ofs) {
    throw std::runtime_error("write_file: cannot open " + abs_path);
  }
  ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!ofs) {
    throw std::runtime_error("write_file: write failed for " + abs_path);
  }
}

// Read <abs_path> back as a binary string. Throws if the file cannot be
// opened.
std::string read_file_to_string(const std::string &abs_path) {
  std::ifstream ifs(vxcore::PathFromUtf8(abs_path), std::ios::binary);
  if (!ifs) {
    throw std::runtime_error("read_file_to_string: cannot open " + abs_path);
  }
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

// 1. After a real file lands in a previously-empty owned folder, the
//    VNote-owned 0-byte .gitkeep MUST be cleaned up on the next sync.
//    The real file stays.
int test_gitkeep_removed_when_real_file_added() {
  std::cout << "  Running test_gitkeep_removed_when_real_file_added..."
            << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "removed"), VXCORE_OK);

    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/A"));

    // First sync: empty A → A/.gitkeep is created (0 bytes, owned).
    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);
    ASSERT_TRUE(keep_exists(f.notebook_root + "/A"));
    ASSERT_EQ(keep_size(f.notebook_root + "/A"),
              static_cast<std::uintmax_t>(0));

    // Add a real note, then sync again.
    write_file(f.notebook_root + "/A/note.md", "real content");
    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    // Marker must be gone, real file must remain.
    ASSERT_FALSE(keep_exists(f.notebook_root + "/A"));
    ASSERT_TRUE(file_exists(f.notebook_root + "/A/note.md"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_gitkeep_removed_when_real_file_added passed"
            << std::endl;
  return 0;
}

// 2. A non-zero-byte .gitkeep authored by the user is user-owned and MUST be
//    preserved verbatim across sync — even though the folder would otherwise
//    look empty from VNote's perspective.
int test_user_authored_nonempty_gitkeep_preserved() {
  std::cout << "  Running test_user_authored_nonempty_gitkeep_preserved..."
            << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "userown"), VXCORE_OK);

    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/B"));
    // EXACT content "keep me" (7 bytes, no trailing newline). The plan's
    // 8-byte claim is a miscount; we follow the literal content string.
    // What matters semantically is size > 0 → user-authored → never deleted.
    write_file(f.notebook_root + "/B/.gitkeep", "keep me");
    ASSERT_EQ(keep_size(f.notebook_root + "/B"),
              static_cast<std::uintmax_t>(7));

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    // Marker must still exist, size unchanged, content byte-identical.
    ASSERT_TRUE(keep_exists(f.notebook_root + "/B"));
    ASSERT_EQ(keep_size(f.notebook_root + "/B"),
              static_cast<std::uintmax_t>(7));
    const std::string roundtripped =
        read_file_to_string(f.notebook_root + "/B/.gitkeep");
    ASSERT_TRUE(roundtripped == std::string("keep me"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_user_authored_nonempty_gitkeep_preserved passed"
            << std::endl;
  return 0;
}

// 3. Idempotent ownership preservation: an owned 0-byte .gitkeep in a still-
//    empty folder must survive a second sync unchanged.
int test_owned_gitkeep_preserved_when_folder_still_empty() {
  std::cout
      << "  Running test_owned_gitkeep_preserved_when_folder_still_empty..."
      << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "ownedidem"), VXCORE_OK);

    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/C"));

    // First sync creates the owned 0-byte marker.
    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);
    ASSERT_TRUE(keep_exists(f.notebook_root + "/C"));
    ASSERT_EQ(keep_size(f.notebook_root + "/C"),
              static_cast<std::uintmax_t>(0));

    // Second sync: folder still empty → marker MUST still exist, still 0 B.
    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);
    ASSERT_TRUE(keep_exists(f.notebook_root + "/C"));
    ASSERT_EQ(keep_size(f.notebook_root + "/C"),
              static_cast<std::uintmax_t>(0));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout
      << "  test_owned_gitkeep_preserved_when_folder_still_empty passed"
      << std::endl;
  return 0;
}

// 4. Default exclude_paths contains "*.vswp" (sync_types.h:65), so a folder
//    whose only child is a swap file counts as effectively empty (Default #5)
//    and MUST receive a .gitkeep.
int test_folder_with_only_vswp_file_gets_gitkeep() {
  std::cout << "  Running test_folder_with_only_vswp_file_gets_gitkeep..."
            << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "vswponly"), VXCORE_OK);

    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/D"));
    // Swap file matches the default exclude glob "*.vswp".
    write_file(f.notebook_root + "/D/scratch.vswp", "swap");

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    ASSERT_TRUE(keep_exists(f.notebook_root + "/D"));
    ASSERT_EQ(keep_size(f.notebook_root + "/D"),
              static_cast<std::uintmax_t>(0));
    // The swap file itself is untouched.
    ASSERT_TRUE(file_exists(f.notebook_root + "/D/scratch.vswp"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_folder_with_only_vswp_file_gets_gitkeep passed"
            << std::endl;
  return 0;
}

// 5. A folder whose only child is a skip-listed subdir (dot-prefix) is
//    effectively empty per Default #2 — it MUST receive a .gitkeep, but
//    the skipped subdir itself MUST NOT.
int test_folder_with_only_skipped_subdir_gets_gitkeep() {
  std::cout << "  Running test_folder_with_only_skipped_subdir_gets_gitkeep..."
            << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "skipsub"), VXCORE_OK);

    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/E/.cache"));

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    ASSERT_TRUE(keep_exists(f.notebook_root + "/E"));
    ASSERT_EQ(keep_size(f.notebook_root + "/E"),
              static_cast<std::uintmax_t>(0));
    // Skipped subdir must not be decorated.
    ASSERT_FALSE(keep_exists(f.notebook_root + "/E/.cache"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_folder_with_only_skipped_subdir_gets_gitkeep passed"
            << std::endl;
  return 0;
}

// 6. A folder whose only child is a real (non-skip-listed) sub-directory is
//    NOT effectively empty, regardless of whether that subdir has its own
//    .gitkeep. F must NOT receive its own marker; the empty leaf F/G does.
int test_folder_with_gitkeep_bearing_subdir_no_own_gitkeep() {
  std::cout
      << "  Running test_folder_with_gitkeep_bearing_subdir_no_own_gitkeep..."
      << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "subkeep"), VXCORE_OK);

    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/F/G"));

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    // Empty leaf gets the marker.
    ASSERT_TRUE(keep_exists(f.notebook_root + "/F/G"));
    ASSERT_EQ(keep_size(f.notebook_root + "/F/G"),
              static_cast<std::uintmax_t>(0));
    // F holds a real subdir (G) → NOT effectively empty → no own marker.
    ASSERT_FALSE(keep_exists(f.notebook_root + "/F"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout
      << "  test_folder_with_gitkeep_bearing_subdir_no_own_gitkeep passed"
      << std::endl;
  return 0;
}

// 7. Cleanup only removes the zero-byte (owned) marker. A non-zero (user-
//    authored) .gitkeep is NEVER deleted, even when the folder is no longer
//    empty. Both sub-scenarios run on side-by-side folders in a single
//    notebook so we cover the size==0 and size>0 ownership branches in one
//    Sync invocation.
int test_cleanup_only_removes_zero_byte_marker() {
  std::cout << "  Running test_cleanup_only_removes_zero_byte_marker..."
            << std::endl;
  Fixture f;
  try {
    ASSERT_EQ(setup_synced_notebook(f, "ownership"), VXCORE_OK);

    // Sub-scenario (a): H has an owned 0-byte marker AND a real note.
    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/H"));
    write_file(f.notebook_root + "/H/.gitkeep", "");
    write_file(f.notebook_root + "/H/note.md", "real");
    ASSERT_EQ(keep_size(f.notebook_root + "/H"),
              static_cast<std::uintmax_t>(0));

    // Sub-scenario (b): I has a user-authored 1-byte marker AND a real note.
    std::filesystem::create_directories(
        vxcore::PathFromUtf8(f.notebook_root + "/I"));
    write_file(f.notebook_root + "/I/.gitkeep", "x");
    write_file(f.notebook_root + "/I/note.md", "real");
    ASSERT_EQ(keep_size(f.notebook_root + "/I"),
              static_cast<std::uintmax_t>(1));

    ASSERT_EQ(f.backend.Sync(nullptr, nullptr), VXCORE_OK);

    // (a) Owned marker deleted; real file kept.
    ASSERT_FALSE(keep_exists(f.notebook_root + "/H"));
    ASSERT_TRUE(file_exists(f.notebook_root + "/H/note.md"));

    // (b) User-authored marker preserved, byte-identical; real file kept.
    ASSERT_TRUE(keep_exists(f.notebook_root + "/I"));
    ASSERT_EQ(keep_size(f.notebook_root + "/I"),
              static_cast<std::uintmax_t>(1));
    const std::string roundtripped =
        read_file_to_string(f.notebook_root + "/I/.gitkeep");
    ASSERT_TRUE(roundtripped == std::string("x"));
    ASSERT_TRUE(file_exists(f.notebook_root + "/I/note.md"));
  } catch (const std::exception &e) {
    std::cerr << "  exception: " << e.what() << std::endl;
    teardown_fixture(f);
    return 1;
  }
  teardown_fixture(f);
  std::cout << "  test_cleanup_only_removes_zero_byte_marker passed"
            << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  RUN_TEST(test_gitkeep_removed_when_real_file_added);
  RUN_TEST(test_user_authored_nonempty_gitkeep_preserved);
  RUN_TEST(test_owned_gitkeep_preserved_when_folder_still_empty);
  RUN_TEST(test_folder_with_only_vswp_file_gets_gitkeep);
  RUN_TEST(test_folder_with_only_skipped_subdir_gets_gitkeep);
  RUN_TEST(test_folder_with_gitkeep_bearing_subdir_no_own_gitkeep);
  RUN_TEST(test_cleanup_only_removes_zero_byte_marker);
  std::cout << "All test_gitkeep_cleanup tests passed" << std::endl;
  return 0;
}
