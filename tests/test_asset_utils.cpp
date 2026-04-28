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

  std::cout << "All asset_utils tests passed!" << std::endl;
  return 0;
}
