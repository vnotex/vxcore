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

int main() {
  std::cout << "Running file_utils tests..." << std::endl;

  RUN_TEST(test_concatenate_paths);
  RUN_TEST(test_split_path);
  RUN_TEST(test_clean_path);
  RUN_TEST(test_relative_path);

  std::cout << "✓ All file_utils tests passed" << std::endl;
  return 0;
}
