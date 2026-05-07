#include <fstream>
#include <iostream>

#include "test_utils.h"
#include "utils/file_utils.h"

int test_concatenate_paths() {
  std::cout << "  Running test_concatenate_paths..." << std::endl;

  ASSERT_EQ(vxcore::ConcatenatePaths("parent", "child"), "parent/child");
  ASSERT_EQ(vxcore::ConcatenatePaths("", "child"), "child");
  ASSERT_EQ(vxcore::ConcatenatePaths(".", "child"), "child");
  ASSERT_EQ(vxcore::ConcatenatePaths("parent", ""), "parent/");

  std::cout << "  ✓ test_concatenate_paths passed" << std::endl;
  return 0;
}

int test_split_path() {
  std::cout << "  Running test_split_path..." << std::endl;

  auto [parent1, child1] = vxcore::SplitPath("parent/child");
  ASSERT_EQ(parent1, "parent");
  ASSERT_EQ(child1, "child");

  auto [parent2, child2] = vxcore::SplitPath("parent\\child");
  ASSERT_EQ(parent2, ".");
  // Do not support `\`.
  ASSERT_EQ(child2, "parent\\child");

  auto [parent3, child3] = vxcore::SplitPath("child");
  ASSERT_EQ(parent3, ".");
  ASSERT_EQ(child3, "child");

  auto [parent4, child4] = vxcore::SplitPath("/parent/child");
  ASSERT_EQ(parent4, "/parent");
  ASSERT_EQ(child4, "child");

  std::cout << "  ✓ test_split_path passed" << std::endl;
  return 0;
}

int test_clean_path() {
  std::cout << "  Running test_clean_path..." << std::endl;

  ASSERT_EQ(vxcore::CleanPath(""), ".");
  ASSERT_EQ(vxcore::CleanPath("."), ".");
  ASSERT_EQ(vxcore::CleanPath("./local"), "local");
  ASSERT_EQ(vxcore::CleanPath("local/../bin"), "bin");
  ASSERT_EQ(vxcore::CleanPath("/local/usr/../bin"), "/local/bin");
  ASSERT_EQ(vxcore::CleanPath("a\\b\\c"), "a/b/c");
  ASSERT_EQ(vxcore::CleanPath("a//b///c"), "a/b/c");
  ASSERT_EQ(vxcore::CleanPath("a/./b"), "a/b");
  ASSERT_EQ(vxcore::CleanPath("../a"), "../a");

  ASSERT_EQ(vxcore::CleanPath("a/b/.."), "a/");
  ASSERT_EQ(vxcore::CleanPath("a/b/../.."), ".");
  ASSERT_EQ(vxcore::CleanPath("/a/b/../.."), "/");
  ASSERT_EQ(vxcore::CleanPath("/../a"), "/a");

  ASSERT_EQ(vxcore::CleanPath("C:\\Users\\test"), "C:/Users/test");
  ASSERT_EQ(vxcore::CleanPath("C:\\Users\\..\\test"), "C:/test");
  ASSERT_EQ(vxcore::CleanPath("C:/Windows/System32"), "C:/Windows/System32");
  ASSERT_EQ(vxcore::CleanPath("C:\\Windows\\..\\..\\test"), "C:/test");
  ASSERT_EQ(vxcore::CleanPath("\\\\server\\share\\path"), "//server/share/path");
  ASSERT_EQ(vxcore::CleanPath("\\\\server\\share\\..\\other"), "//server/other");
  ASSERT_EQ(vxcore::CleanPath("C:\\"), "C:/");
  ASSERT_EQ(vxcore::CleanPath("C:\\.\\Users"), "C:/Users");
  ASSERT_EQ(vxcore::CleanPath("D:\\folder\\subfolder\\..\\file.txt"), "D:/folder/file.txt");

  std::cout << "  ✓ test_clean_path passed" << std::endl;
  return 0;
}

int test_relative_path() {
  std::cout << "  Running test_relative_path..." << std::endl;

  ASSERT_EQ(vxcore::RelativePath("/home/user", "/home/user/docs/file.txt"), "docs/file.txt");
  ASSERT_EQ(vxcore::RelativePath("/home/user/", "/home/user/docs/file.txt"), "docs/file.txt");
  ASSERT_EQ(vxcore::RelativePath("/home/user", "/home/user"), "");
  ASSERT_EQ(vxcore::RelativePath("/home/user", "/home/other/file.txt"), "");
  ASSERT_EQ(vxcore::RelativePath("C:/Users/test", "C:/Users/test/Documents/file.txt"),
            "Documents/file.txt");
  ASSERT_EQ(vxcore::RelativePath("C:/Users/test", "D:/Users/test/file.txt"), "");
  ASSERT_EQ(vxcore::RelativePath("", "/home/user"), std::string());
  ASSERT_EQ(vxcore::RelativePath("/home/user", ""), ".");

  std::cout << "  ✓ test_relative_path passed" << std::endl;
  return 0;
}

int test_path_exists() {
  std::cout << "  Running test_path_exists..." << std::endl;

  // Setup: create temp dir with non-ASCII subdirectory
  std::string base = get_test_path("test_path_exists");
  cleanup_test_dir(base);
  create_directory(base);

  // Create non-ASCII directory and file using PathFromUtf8 (test_utils helpers are ANSI-unsafe)
  std::string utf8_dir = base + "/测试目录";
  std::string utf8_file = utf8_dir + "/テスト.md";
  std::filesystem::create_directories(vxcore::PathFromUtf8(utf8_dir));
  {
    std::ofstream f(vxcore::PathFromUtf8(utf8_file));
    f << "test content";
  }

  // ASCII paths
  std::string ascii_file = base + "/ascii.txt";
  write_file(ascii_file, "hello");

  // Test: existing paths
  ASSERT_TRUE(vxcore::PathExists(base));
  ASSERT_TRUE(vxcore::PathExists(utf8_dir));
  ASSERT_TRUE(vxcore::PathExists(utf8_file));
  ASSERT_TRUE(vxcore::PathExists(ascii_file));

  // Test: non-existent paths
  ASSERT_FALSE(vxcore::PathExists(base + "/nonexistent"));
  ASSERT_FALSE(vxcore::PathExists(base + "/不存在的路径"));

  // Test: empty string
  ASSERT_FALSE(vxcore::PathExists(""));

  cleanup_test_dir(base);
  std::cout << "  ✓ test_path_exists passed" << std::endl;
  return 0;
}

int test_is_directory() {
  std::cout << "  Running test_is_directory..." << std::endl;

  std::string base = get_test_path("test_is_directory");
  cleanup_test_dir(base);
  create_directory(base);

  // Create non-ASCII directory and file
  std::string utf8_dir = base + "/文件夹";
  std::string utf8_file = base + "/文件.md";
  std::filesystem::create_directories(vxcore::PathFromUtf8(utf8_dir));
  {
    std::ofstream f(vxcore::PathFromUtf8(utf8_file));
    f << "content";
  }

  // Directories return true
  ASSERT_TRUE(vxcore::IsDirectory(base));
  ASSERT_TRUE(vxcore::IsDirectory(utf8_dir));

  // Files return false
  ASSERT_FALSE(vxcore::IsDirectory(utf8_file));

  // Non-existent returns false
  ASSERT_FALSE(vxcore::IsDirectory(base + "/不存在"));

  // Empty returns false
  ASSERT_FALSE(vxcore::IsDirectory(""));

  cleanup_test_dir(base);
  std::cout << "  ✓ test_is_directory passed" << std::endl;
  return 0;
}

int test_is_regular_file() {
  std::cout << "  Running test_is_regular_file..." << std::endl;

  std::string base = get_test_path("test_is_regular_file");
  cleanup_test_dir(base);
  create_directory(base);

  std::string utf8_dir = base + "/目录";
  std::string utf8_file = base + "/笔记.md";
  std::filesystem::create_directories(vxcore::PathFromUtf8(utf8_dir));
  {
    std::ofstream f(vxcore::PathFromUtf8(utf8_file));
    f << "note content";
  }

  // Files return true
  ASSERT_TRUE(vxcore::IsRegularFile(utf8_file));

  // Directories return false
  ASSERT_FALSE(vxcore::IsRegularFile(utf8_dir));
  ASSERT_FALSE(vxcore::IsRegularFile(base));

  // Non-existent returns false
  ASSERT_FALSE(vxcore::IsRegularFile(base + "/不存在.md"));

  // Empty returns false
  ASSERT_FALSE(vxcore::IsRegularFile(""));

  cleanup_test_dir(base);
  std::cout << "  ✓ test_is_regular_file passed" << std::endl;
  return 0;
}

int test_path_filename() {
  std::cout << "  Running test_path_filename..." << std::endl;

  // PathFilename extracts filename from path string — no filesystem access needed
  ASSERT_EQ(vxcore::PathFilename("parent/child.md"), std::string("child.md"));
  ASSERT_EQ(vxcore::PathFilename("a/b/c/工具使用/其他技巧.md"), std::string("其他技巧.md"));
  ASSERT_EQ(vxcore::PathFilename("テスト.md"), std::string("テスト.md"));
  ASSERT_EQ(vxcore::PathFilename("/root/笔记/日记.txt"), std::string("日记.txt"));

  // Edge cases
  ASSERT_EQ(vxcore::PathFilename(""), std::string(""));
  ASSERT_EQ(vxcore::PathFilename("nopath"), std::string("nopath"));

  std::cout << "  ✓ test_path_filename passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running file_utils tests..." << std::endl;

  RUN_TEST(test_concatenate_paths);
  RUN_TEST(test_split_path);
  RUN_TEST(test_clean_path);
  RUN_TEST(test_relative_path);
  RUN_TEST(test_path_exists);
  RUN_TEST(test_is_directory);
  RUN_TEST(test_is_regular_file);
  RUN_TEST(test_path_filename);

  std::cout << "✓ All file_utils tests passed" << std::endl;
  return 0;
}
