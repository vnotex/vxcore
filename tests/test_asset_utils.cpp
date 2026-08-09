#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/content_processor/asset_utils.h"
#include "utils/file_utils.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace fs = std::filesystem;

static int test_copy_assets_directory() {
  std::string base = get_test_path("asset_utils_copy");
  cleanup_test_dir(base);

  std::string src = base + "/src_assets";
  std::string dest = base + "/dest_assets";

  create_directory(src);
  write_file(src + "/image.png", "png_data");
  create_directory(src + "/sub");
  write_file(src + "/sub/deep.txt", "deep_data");

  VxCoreError err = vxcore::CopyAssetsDirectory(src, dest);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify dest has files
  ASSERT_TRUE(path_exists(dest + "/image.png"));
  ASSERT_TRUE(path_exists(dest + "/sub/deep.txt"));

  // Verify source unchanged
  ASSERT_TRUE(path_exists(src + "/image.png"));
  ASSERT_TRUE(path_exists(src + "/sub/deep.txt"));

  cleanup_test_dir(base);
  return 0;
}

static int test_copy_assets_nonexistent_source() {
  std::string base = get_test_path("asset_utils_copy_noexist");
  cleanup_test_dir(base);

  std::string src = base + "/no_such_dir";
  std::string dest = base + "/dest_assets";

  VxCoreError err = vxcore::CopyAssetsDirectory(src, dest);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_FALSE(path_exists(dest));

  cleanup_test_dir(base);
  return 0;
}

static int test_move_assets_directory() {
  std::string base = get_test_path("asset_utils_move");
  cleanup_test_dir(base);

  std::string src = base + "/src_assets";
  std::string dest = base + "/dest_assets";

  create_directory(src);
  write_file(src + "/file1.txt", "data1");
  write_file(src + "/file2.txt", "data2");

  VxCoreError err = vxcore::MoveAssetsDirectory(src, dest);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify dest has files
  ASSERT_TRUE(path_exists(dest + "/file1.txt"));
  ASSERT_TRUE(path_exists(dest + "/file2.txt"));

  // Verify source gone
  ASSERT_FALSE(path_exists(src));

  cleanup_test_dir(base);
  return 0;
}

static int test_move_assets_nonexistent_source() {
  std::string base = get_test_path("asset_utils_move_noexist");
  cleanup_test_dir(base);

  std::string src = base + "/no_such_dir";
  std::string dest = base + "/dest_assets";

  VxCoreError err = vxcore::MoveAssetsDirectory(src, dest);
  ASSERT_EQ(err, VXCORE_OK);

  cleanup_test_dir(base);
  return 0;
}

static int test_copy_relative_linked_nested() {
  std::string base = get_test_path("rel_copy_nested");
  cleanup_test_dir(base);

  std::string notebook_root = base + "/notebook";
  std::string src_dir = notebook_root + "/folder_a";
  std::string dest_dir = notebook_root + "/folder_b";
  std::string source_file = src_dir + "/note.md";

  create_directory(src_dir + "/images/sub");
  write_file(src_dir + "/images/sub/pic.png", "png_data");
  write_file(source_file, "# note");

  std::vector<std::string> paths = {"images/sub/pic.png"};
  int count = vxcore::CopyRelativeLinkedFiles(
      paths, src_dir, dest_dir,
      vxcore::CleanPath(notebook_root), vxcore::CleanPath(source_file));
  ASSERT_EQ(count, 1);
  ASSERT_TRUE(path_exists(dest_dir + "/images/sub/pic.png"));

  cleanup_test_dir(base);
  return 0;
}

static int test_copy_relative_preserves_source() {
  std::string base = get_test_path("rel_copy_preserve");
  cleanup_test_dir(base);

  std::string notebook_root = base + "/notebook";
  std::string src_dir = notebook_root + "/a";
  std::string dest_dir = notebook_root + "/b";

  create_directory(src_dir);
  write_file(src_dir + "/file.txt", "data");

  std::vector<std::string> paths = {"file.txt"};
  vxcore::CopyRelativeLinkedFiles(
      paths, src_dir, dest_dir,
      vxcore::CleanPath(notebook_root), vxcore::CleanPath(src_dir + "/note.md"));

  ASSERT_TRUE(path_exists(src_dir + "/file.txt"));
  ASSERT_TRUE(path_exists(dest_dir + "/file.txt"));

  cleanup_test_dir(base);
  return 0;
}

static int test_move_relative_basic() {
  std::string base = get_test_path("rel_move_basic");
  cleanup_test_dir(base);

  std::string notebook_root = base + "/notebook";
  std::string src_dir = notebook_root + "/a";
  std::string dest_dir = notebook_root + "/b";

  create_directory(src_dir);
  write_file(src_dir + "/file.txt", "data");

  std::vector<std::string> paths = {"file.txt"};
  int count = vxcore::MoveRelativeLinkedFiles(
      paths, src_dir, dest_dir,
      vxcore::CleanPath(notebook_root), vxcore::CleanPath(src_dir + "/note.md"));
  ASSERT_EQ(count, 1);
  ASSERT_TRUE(path_exists(dest_dir + "/file.txt"));
  ASSERT_FALSE(path_exists(src_dir + "/file.txt"));

  cleanup_test_dir(base);
  return 0;
}

static int test_copy_relative_skip_nonexistent() {
  std::string base = get_test_path("rel_copy_noexist");
  cleanup_test_dir(base);

  std::string notebook_root = base + "/notebook";
  std::string src_dir = notebook_root + "/a";
  std::string dest_dir = notebook_root + "/b";
  create_directory(src_dir);

  std::vector<std::string> paths = {"no_such_file.txt"};
  int count = vxcore::CopyRelativeLinkedFiles(
      paths, src_dir, dest_dir,
      vxcore::CleanPath(notebook_root), vxcore::CleanPath(src_dir + "/note.md"));
  ASSERT_EQ(count, 0);

  cleanup_test_dir(base);
  return 0;
}

static int test_copy_relative_skip_outside_notebook() {
  std::string base = get_test_path("rel_copy_outside");
  cleanup_test_dir(base);

  std::string notebook_root = base + "/notebook";
  std::string src_dir = notebook_root + "/a";
  std::string dest_dir = notebook_root + "/b";
  std::string outside = base + "/outside";

  create_directory(src_dir);
  create_directory(outside);
  write_file(outside + "/secret.txt", "secret");

  // ../../outside/secret.txt resolves outside notebook_root
  std::vector<std::string> paths = {"../../outside/secret.txt"};
  int count = vxcore::CopyRelativeLinkedFiles(
      paths, src_dir, dest_dir,
      vxcore::CleanPath(notebook_root), vxcore::CleanPath(src_dir + "/note.md"));
  ASSERT_EQ(count, 0);

  cleanup_test_dir(base);
  return 0;
}

static int test_copy_relative_skip_self_link() {
  std::string base = get_test_path("rel_copy_self");
  cleanup_test_dir(base);

  std::string notebook_root = base + "/notebook";
  std::string src_dir = notebook_root + "/a";
  std::string dest_dir = notebook_root + "/b";
  std::string source_file = src_dir + "/note.md";

  create_directory(src_dir);
  write_file(source_file, "# self");

  std::vector<std::string> paths = {"note.md"};
  int count = vxcore::CopyRelativeLinkedFiles(
      paths, src_dir, dest_dir,
      vxcore::CleanPath(notebook_root), vxcore::CleanPath(source_file));
  ASSERT_EQ(count, 0);

  cleanup_test_dir(base);
  return 0;
}

static int test_copy_relative_skip_existing() {
  std::string base = get_test_path("rel_copy_skipexist");
  cleanup_test_dir(base);

  std::string notebook_root = base + "/notebook";
  std::string src_dir = notebook_root + "/a";
  std::string dest_dir = notebook_root + "/b";

  create_directory(src_dir);
  create_directory(dest_dir);
  write_file(src_dir + "/file.txt", "new_data");
  write_file(dest_dir + "/file.txt", "old_data");

  std::vector<std::string> paths = {"file.txt"};
  int count = vxcore::CopyRelativeLinkedFiles(
      paths, src_dir, dest_dir,
      vxcore::CleanPath(notebook_root), vxcore::CleanPath(src_dir + "/note.md"));
  // skip_existing means it "succeeds" but doesn't overwrite
  ASSERT_EQ(count, 1);
  // Verify old data preserved — read dest file
  std::ifstream ifs(dest_dir + "/file.txt");
  std::string content((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
  ASSERT_EQ(content, "old_data");

  cleanup_test_dir(base);
  return 0;
}

// Sanity check that the CJK literals below were compiled as UTF-8 (see the
// directory-wide /utf-8 in tests/CMakeLists.txt). A bare "has a byte >= 0x80"
// check would be useless because mojibake is also non-ASCII, so pin the exact
// UTF-8 encoding of a known literal: U+4E34 U+65F6 ("临时") is E4 B8 B4 E6 97 B6.
// NOTE: this is a sanity check, NOT a reliable detector of a missing /utf-8 —
// a single-byte ACP can decode these bytes to mojibake and re-encode them back
// to the same bytes. The build-system flag is the actual guarantee.
static int test_cjk_fixtures_are_utf8() {
  const std::string literal = "临时";
  const std::string expected = "\xE4\xB8\xB4\xE6\x97\xB6";
  ASSERT_EQ(literal, expected);
  return 0;
}

// Regression: issue #2729. Every std::filesystem boundary in asset_utils must
// go through PathFromUtf8(). Passing a UTF-8 std::string straight to
// std::filesystem makes MSVC convert it with the active ANSI code page, which
// THROWS std::system_error when the path holds characters the ACP cannot
// encode (CJK on a Western-ACP machine). The escaping exception aborted
// BundledFolderManager::MoveFile after the file had already been renamed but
// before the folder metadata was updated, leaving disk and vx.json divergent.
static int test_move_assets_directory_non_ascii_path() {
  std::string base = get_test_path("asset_utils_move_非ASCII");
  cleanup_test_dir(base);

  std::string src = base + "/临时文件夹（自动清空）/vx_assets/笔记";
  std::string dest = base + "/02_精品收集/vx_assets/笔记";

  create_directory(src);
  write_file(src + "/图片.png", "png_data");

  // Must not throw, and must report success.
  VxCoreError err = vxcore::MoveAssetsDirectory(src, dest);
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT_TRUE(path_exists(dest + "/图片.png"));
  ASSERT_FALSE(path_exists(src));

  cleanup_test_dir(base);
  return 0;
}

// Same bug class on the copy path.
static int test_copy_assets_directory_non_ascii_path() {
  std::string base = get_test_path("asset_utils_copy_非ASCII");
  cleanup_test_dir(base);

  std::string src = base + "/个人记事/vx_assets/笔记";
  std::string dest = base + "/日常记事/vx_assets/笔记";

  create_directory(src);
  write_file(src + "/附件.txt", "data");

  VxCoreError err = vxcore::CopyAssetsDirectory(src, dest);
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT_TRUE(path_exists(dest + "/附件.txt"));
  ASSERT_TRUE(path_exists(src + "/附件.txt"));

  cleanup_test_dir(base);
  return 0;
}

// A non-ASCII source that does NOT exist must still return OK (the no-assets
// case that runs on every single file move) rather than throwing.
static int test_move_assets_non_ascii_nonexistent_source() {
  std::string base = get_test_path("asset_utils_move_noexist_非ASCII");
  cleanup_test_dir(base);

  std::string src = base + "/临时文件夹（自动清空）/vx_assets/不存在";
  std::string dest = base + "/02_精品收集/vx_assets/不存在";

  VxCoreError err = vxcore::MoveAssetsDirectory(src, dest);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_FALSE(path_exists(dest));

  cleanup_test_dir(base);
  return 0;
}

// MoveRelativeLinkedFiles walks the same boundaries.
static int test_move_relative_linked_non_ascii_path() {
  std::string base = get_test_path("asset_utils_rel_非ASCII");
  cleanup_test_dir(base);

  std::string notebook_root = base + "/个人记事-v4";
  std::string src_dir = notebook_root + "/02_精品收集";
  std::string dest_dir = notebook_root + "/01_日常记事";

  create_directory(src_dir);
  create_directory(dest_dir);
  write_file(src_dir + "/图片.png", "png_data");

  std::vector<std::string> paths = {"图片.png"};
  int count = vxcore::MoveRelativeLinkedFiles(
      paths, src_dir, dest_dir, vxcore::CleanPath(notebook_root),
      vxcore::CleanPath(src_dir + "/面试与离职.md"));
  ASSERT_EQ(count, 1);

  ASSERT_TRUE(path_exists(dest_dir + "/图片.png"));
  ASSERT_FALSE(path_exists(src_dir + "/图片.png"));

  cleanup_test_dir(base);
  return 0;
}

int main() {
  vxcore_set_test_mode(1);

  RUN_TEST(test_copy_assets_directory);
  RUN_TEST(test_copy_assets_nonexistent_source);
  RUN_TEST(test_move_assets_directory);
  RUN_TEST(test_move_assets_nonexistent_source);
  RUN_TEST(test_copy_relative_linked_nested);
  RUN_TEST(test_copy_relative_preserves_source);
  RUN_TEST(test_move_relative_basic);
  RUN_TEST(test_copy_relative_skip_nonexistent);
  RUN_TEST(test_copy_relative_skip_outside_notebook);
  RUN_TEST(test_copy_relative_skip_self_link);
  RUN_TEST(test_copy_relative_skip_existing);
  RUN_TEST(test_cjk_fixtures_are_utf8);
  RUN_TEST(test_move_assets_directory_non_ascii_path);
  RUN_TEST(test_copy_assets_directory_non_ascii_path);
  RUN_TEST(test_move_assets_non_ascii_nonexistent_source);
  RUN_TEST(test_move_relative_linked_non_ascii_path);

  std::cout << "All asset_utils tests passed!" << std::endl;
  return 0;
}
