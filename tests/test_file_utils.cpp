#include <filesystem>
#include <fstream>
#include <iostream>

#include "test_reparse_utils.h"
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
  ASSERT_EQ(vxcore::CleanPath("a//b///c"), "a/b/c");
  ASSERT_EQ(vxcore::CleanPath("a/./b"), "a/b");
  ASSERT_EQ(vxcore::CleanPath("../a"), "../a");

  ASSERT_EQ(vxcore::CleanPath("a/b/.."), "a/");
  ASSERT_EQ(vxcore::CleanPath("a/b/../.."), ".");
  ASSERT_EQ(vxcore::CleanPath("/a/b/../.."), "/");
  ASSERT_EQ(vxcore::CleanPath("/../a"), "/a");

  ASSERT_EQ(vxcore::CleanPath("C:/Windows/System32"), "C:/Windows/System32");

  // Backslash and drive-letter handling is inherently platform-dependent:
  // CleanPath normalizes through std::filesystem::path, where '\' is a
  // separator (and "C:" a root name) ONLY on Windows. On POSIX a backslash is
  // an ordinary filename character, so such inputs are single components and
  // come back verbatim. Assert the real behavior on each platform rather than
  // the Windows one everywhere.
#ifdef _WIN32
  ASSERT_EQ(vxcore::CleanPath("a\\b\\c"), "a/b/c");
  ASSERT_EQ(vxcore::CleanPath("C:\\Users\\test"), "C:/Users/test");
  ASSERT_EQ(vxcore::CleanPath("C:\\Users\\..\\test"), "C:/test");
  ASSERT_EQ(vxcore::CleanPath("C:\\Windows\\..\\..\\test"), "C:/test");
  ASSERT_EQ(vxcore::CleanPath("\\\\server\\share\\path"), "//server/share/path");
  ASSERT_EQ(vxcore::CleanPath("\\\\server\\share\\..\\other"), "//server/other");
  ASSERT_EQ(vxcore::CleanPath("C:\\"), "C:/");
  ASSERT_EQ(vxcore::CleanPath("C:\\.\\Users"), "C:/Users");
  ASSERT_EQ(vxcore::CleanPath("D:\\folder\\subfolder\\..\\file.txt"), "D:/folder/file.txt");
#else
  ASSERT_EQ(vxcore::CleanPath("a\\b\\c"), "a\\b\\c");
  ASSERT_EQ(vxcore::CleanPath("C:\\Users\\test"), "C:\\Users\\test");
  ASSERT_EQ(vxcore::CleanPath("C:\\Users\\..\\test"), "C:\\Users\\..\\test");
  ASSERT_EQ(vxcore::CleanPath("C:\\Windows\\..\\..\\test"), "C:\\Windows\\..\\..\\test");
  ASSERT_EQ(vxcore::CleanPath("\\\\server\\share\\path"), "\\\\server\\share\\path");
  ASSERT_EQ(vxcore::CleanPath("\\\\server\\share\\..\\other"), "\\\\server\\share\\..\\other");
  ASSERT_EQ(vxcore::CleanPath("C:\\"), "C:\\");
  ASSERT_EQ(vxcore::CleanPath("C:\\.\\Users"), "C:\\.\\Users");
  ASSERT_EQ(vxcore::CleanPath("D:\\folder\\subfolder\\..\\file.txt"),
            "D:\\folder\\subfolder\\..\\file.txt");
#endif

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

int test_is_path_within() {
  std::cout << "  Running test_is_path_within..." << std::endl;

  std::string base = get_test_path("test_is_path_within");
  cleanup_test_dir(base);
  create_directory(base);

  std::string inside = base + "/子目录";
  std::string deeper = inside + "/更深/文件.md";
  std::filesystem::create_directories(vxcore::PathFromUtf8(deeper).parent_path());
  {
    std::ofstream f(vxcore::PathFromUtf8(deeper));
    f << "x";
  }

  // Positive: direct child and deep descendant.
  ASSERT_TRUE(vxcore::IsPathWithin(base, inside, false));
  ASSERT_TRUE(vxcore::IsPathWithin(base, deeper, false));

  // Equal path counts as within.
  ASSERT_TRUE(vxcore::IsPathWithin(base, base, false));

  // Negative: sibling and parent.
  std::string sibling = base + "_other";
  std::filesystem::create_directories(vxcore::PathFromUtf8(sibling));
  ASSERT_FALSE(vxcore::IsPathWithin(base, sibling, true));
  ASSERT_FALSE(vxcore::IsPathWithin(inside, base, true));

  // A path that cannot be verified returns |on_error| in BOTH directions.
  // This is what proves the ImportFolder guard now fails CLOSED: an
  // unverifiable path is treated as "inside the notebook root" and rejected
  // rather than silently proceeding with the guard disabled.
  ASSERT_TRUE(vxcore::IsPathWithin(base, "", true));
  ASSERT_FALSE(vxcore::IsPathWithin(base, "", false));
  ASSERT_TRUE(vxcore::IsPathWithin("", base, true));
  ASSERT_FALSE(vxcore::IsPathWithin("", base, false));

#ifndef _WIN32
  // A genuine weakly_canonical() failure: a symlink cycle resolves to ELOOP.
  // (Windows has no portable way to make weakly_canonical set error_code -
  // over-long paths, invalid characters and empty paths all succeed there -
  // so the ec branch is covered on POSIX only.)
  const std::string loop_a = base + "/loop_a";
  const std::string loop_b = base + "/loop_b";
  std::error_code link_ec;
  std::filesystem::create_symlink(vxcore::PathFromUtf8(loop_b), vxcore::PathFromUtf8(loop_a),
                                  link_ec);
  if (!link_ec) {
    std::filesystem::create_symlink(vxcore::PathFromUtf8(loop_a), vxcore::PathFromUtf8(loop_b),
                                    link_ec);
  }
  if (!link_ec) {
    std::error_code probe_ec;
    std::filesystem::weakly_canonical(vxcore::PathFromUtf8(loop_a), probe_ec);
    ASSERT_TRUE(static_cast<bool>(probe_ec));
    ASSERT_TRUE(vxcore::IsPathWithin(base, loop_a, true));
    ASSERT_FALSE(vxcore::IsPathWithin(base, loop_a, false));
    ASSERT_TRUE(vxcore::IsPathWithin(loop_a, base, true));
    ASSERT_FALSE(vxcore::IsPathWithin(loop_a, base, false));
  }
  std::error_code rm_ec;
  std::filesystem::remove(vxcore::PathFromUtf8(loop_a), rm_ec);
  std::filesystem::remove(vxcore::PathFromUtf8(loop_b), rm_ec);
#endif

  std::filesystem::remove_all(vxcore::PathFromUtf8(sibling));
  cleanup_test_dir(base);
  std::cout << "  ✓ test_is_path_within passed" << std::endl;
  return 0;
}

int test_is_reparse_point() {
  std::cout << "  Running test_is_reparse_point..." << std::endl;

  std::string base = get_test_path("test_is_reparse_point");
  cleanup_test_dir(base);
  create_directory(base);

  std::string plain_dir = base + "/普通目录";
  std::string plain_file = base + "/普通文件.md";
  std::string target_dir = base + "/目标";
  std::filesystem::create_directories(vxcore::PathFromUtf8(plain_dir));
  std::filesystem::create_directories(vxcore::PathFromUtf8(target_dir));
  {
    std::ofstream f(vxcore::PathFromUtf8(plain_file));
    f << "content";
  }

  // Plain directory/file are not reparse points.
  ASSERT_FALSE(vxcore::IsReparsePoint(plain_dir, true));
  ASSERT_FALSE(vxcore::IsReparsePoint(plain_file, true));

  // Non-existent path returns |on_error|.
  ASSERT_TRUE(vxcore::IsReparsePoint(base + "/不存在", true));
  ASSERT_FALSE(vxcore::IsReparsePoint(base + "/不存在", false));
  ASSERT_TRUE(vxcore::IsReparsePoint("", true));

  // A real junction MUST be detected. Junction creation needs no privilege, so
  // this case is never skipped: it is the case fs::is_symlink() misses on MSVC.
  std::string junction = base + "/连接点";
  ASSERT_TRUE(vxcore_test::create_junction(target_dir, junction));
  ASSERT_TRUE(vxcore::IsReparsePoint(junction, false));
  vxcore_test::remove_reparse_dir(junction);

  // Symlinks too, when the platform lets us create one.
  std::string symlink = base + "/软链接";
  if (vxcore_test::try_create_directory_symlink(target_dir, symlink)) {
    ASSERT_TRUE(vxcore::IsReparsePoint(symlink, false));
    vxcore_test::remove_reparse_dir(symlink);
  } else {
    std::cout << "    (notice: directory symlink creation unavailable; case skipped)"
              << std::endl;
  }

  cleanup_test_dir(base);
  std::cout << "  ✓ test_is_reparse_point passed" << std::endl;
  return 0;
}

int test_copy_tree_skip_reparse_points() {
  std::cout << "  Running test_copy_tree_skip_reparse_points..." << std::endl;

  std::string base = get_test_path("test_copy_tree_skip_reparse");
  cleanup_test_dir(base);
  create_directory(base);

  std::string outside = base + "/外部机密";
  std::filesystem::create_directories(vxcore::PathFromUtf8(outside));
  {
    std::ofstream f(vxcore::PathFromUtf8(outside + "/secret.txt"));
    f << "secret";
  }

  std::string src = base + "/源";
  std::filesystem::create_directories(vxcore::PathFromUtf8(src + "/子目录"));
  {
    std::ofstream f(vxcore::PathFromUtf8(src + "/正常.md"));
    f << "normal";
  }
  {
    std::ofstream f(vxcore::PathFromUtf8(src + "/子目录/嵌套.md"));
    f << "nested";
  }
  ASSERT_TRUE(vxcore_test::create_junction(outside, src + "/逃逸"));

  std::string dest = base + "/目标";
  ASSERT_TRUE(vxcore::CopyTreeSkipReparsePoints(vxcore::PathFromUtf8(src),
                                                vxcore::PathFromUtf8(dest)));

  // Normal content copied.
  ASSERT_TRUE(vxcore::IsRegularFile(dest + "/正常.md"));
  ASSERT_TRUE(vxcore::IsRegularFile(dest + "/子目录/嵌套.md"));
  // Reparse point neither copied nor followed.
  ASSERT_FALSE(vxcore::PathExists(dest + "/逃逸"));
  ASSERT_FALSE(vxcore::PathExists(dest + "/逃逸/secret.txt"));

  vxcore_test::remove_reparse_dir(src + "/逃逸");

  // Error propagation: a missing source and a reparse-point source both report
  // failure rather than a silent "successful" empty copy.
  ASSERT_FALSE(vxcore::CopyTreeSkipReparsePoints(vxcore::PathFromUtf8(base + "/不存在"),
                                                 vxcore::PathFromUtf8(base + "/目标2")));
  std::string link_src = base + "/链接源";
  ASSERT_TRUE(vxcore_test::create_junction(outside, link_src));
  ASSERT_FALSE(vxcore::CopyTreeSkipReparsePoints(vxcore::PathFromUtf8(link_src),
                                                 vxcore::PathFromUtf8(base + "/目标3")));
  ASSERT_FALSE(vxcore::PathExists(base + "/目标3/secret.txt"));
  vxcore_test::remove_reparse_dir(link_src);

  cleanup_test_dir(base);
  std::cout << "  ✓ test_copy_tree_skip_reparse_points passed" << std::endl;
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
  RUN_TEST(test_is_path_within);
  RUN_TEST(test_is_reparse_point);
  RUN_TEST(test_copy_tree_skip_reparse_points);

  std::cout << "✓ All file_utils tests passed" << std::endl;
  return 0;
}
