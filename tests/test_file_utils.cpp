#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

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

int test_atomic_file_publication() {
  const auto base = std::filesystem::temp_directory_path() /
                    ("vxcore_atomic_" + std::to_string(
                        std::chrono::steady_clock::now().time_since_epoch().count()));
  ASSERT_TRUE(std::filesystem::create_directory(base));
  const auto destination = base / vxcore::PathFromUtf8(u8"密钥.vne");
  const std::string original("old\0\r\n", 6);
  const std::string replacement("new\0\r\n", 6);
  auto read_binary = [](const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };

  ASSERT_EQ(vxcore::WriteFileAtomic(destination, original), VXCORE_OK);
  {
    vxcore::AtomicFileWriter writer(destination);
    ASSERT_EQ(writer.Open(), VXCORE_OK);
    ASSERT_EQ(writer.Write(replacement.data(), 2), VXCORE_OK);
    ASSERT_EQ(writer.Write(replacement.data() + 2, replacement.size() - 2), VXCORE_OK);
    ASSERT_EQ(read_binary(destination), original);
    ASSERT_EQ(writer.Commit(), VXCORE_OK);
  }
  ASSERT_EQ(read_binary(destination), replacement);

  // An abandoned or failed write must not publish its partially written bytes.
  {
    vxcore::AtomicFileWriter writer(destination);
    ASSERT_EQ(writer.Open(), VXCORE_OK);
    ASSERT_EQ(writer.Write(original.data(), original.size()), VXCORE_OK);
  }
  ASSERT_EQ(read_binary(destination), replacement);
  {
    vxcore::AtomicFileWriter writer(destination);
    ASSERT_EQ(writer.Open(), VXCORE_OK);
    ASSERT_EQ(writer.Write(original.data(), original.size()), VXCORE_OK);
    ASSERT_EQ(writer.Write(nullptr, 1), VXCORE_ERR_NULL_POINTER);
    ASSERT_EQ(writer.Commit(), VXCORE_ERR_NULL_POINTER);
  }
  ASSERT_EQ(read_binary(destination), replacement);
  ASSERT_EQ(std::distance(std::filesystem::directory_iterator(base),
                          std::filesystem::directory_iterator()), 1);

  // A real replacement error must preserve the existing destination tree.
  const auto blocked = base / "blocked";
  ASSERT_TRUE(std::filesystem::create_directory(blocked));
  ASSERT_EQ(vxcore::WriteFileAtomic(blocked / "keep", original), VXCORE_OK);
  ASSERT_EQ(vxcore::WriteFileAtomic(blocked, replacement), VXCORE_ERR_IO);
  ASSERT_EQ(read_binary(blocked / "keep"), original);
  ASSERT_EQ(vxcore::WriteFileAtomic(base / "missing" / "file", replacement), VXCORE_ERR_IO);
  ASSERT_FALSE(std::filesystem::exists(base / "missing"));
  ASSERT_EQ(vxcore::WriteFileAtomic(destination, std::string()), VXCORE_OK);
  ASSERT_EQ(std::filesystem::file_size(destination), 0);

#ifdef _WIN32
  // The valid destination fits MAX_PATH; only the writer-owned suffix crosses it.
  const auto prefixLength = base.native().size() + 1;
  ASSERT_TRUE(prefixLength < 253);
  const auto longDestination = base / std::wstring(253 - prefixLength, L'x');
  {
    std::ofstream seed(longDestination, std::ios::binary);
    seed.write(original.data(), static_cast<std::streamsize>(original.size()));
    seed.close();
    ASSERT_TRUE(seed.good());
  }
  {
    vxcore::AtomicFileWriter writer(longDestination);
    ASSERT_EQ(writer.Open(), VXCORE_OK);
    ASSERT_EQ(writer.Write(replacement.data(), replacement.size()), VXCORE_OK);
    ASSERT_EQ(read_binary(longDestination), original);
    ASSERT_EQ(writer.Commit(), VXCORE_OK);
  }
  ASSERT_EQ(read_binary(longDestination), replacement);
  {
    vxcore::AtomicFileWriter abandoned(longDestination);
    ASSERT_EQ(abandoned.Open(), VXCORE_OK);
    ASSERT_EQ(abandoned.Write(original.data(), original.size()), VXCORE_OK);
  }
  ASSERT_EQ(read_binary(longDestination), replacement);
  for (const auto &entry : std::filesystem::directory_iterator(base)) {
    ASSERT_TRUE(entry.path().filename().native().find(L".tmp-") == std::wstring::npos);
  }
#endif
  std::filesystem::remove_all(base);
  return 0;
}

int test_no_replace_publication() {
  namespace fs = std::filesystem;
  const auto base = vxcore::PathFromUtf8(get_test_path("no_replace_publication"));
  fs::remove_all(base);
  ASSERT_TRUE(fs::create_directories(base));
  const std::string source_bytes("source\0\xff\r\n", 10);
  const std::string winner_bytes("winner\0\x80\n", 9);
  auto read_binary = [](const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };
  const auto source = base / "source.bin";
  const auto winner = base / "winner.bin";
  ASSERT_EQ(vxcore::WriteFileAtomic(source, source_bytes), VXCORE_OK);
  ASSERT_EQ(vxcore::WriteFileAtomic(winner, winner_bytes), VXCORE_OK);
  vxcore::FileIdentity before, after;
  ASSERT_EQ(vxcore::GetFileIdentity(winner, before), VXCORE_OK);
  ASSERT_NE(vxcore::RenameNoReplace(source, winner), VXCORE_OK);
  ASSERT_EQ(read_binary(source), source_bytes);
  ASSERT_EQ(read_binary(winner), winner_bytes);
  ASSERT_EQ(vxcore::GetFileIdentity(winner, after), VXCORE_OK);
  ASSERT_TRUE(before.SameObject(after));
  ASSERT_EQ(vxcore::GetFileIdentity(source, before), VXCORE_OK);
  const auto moved = base / "moved.bin";
  ASSERT_EQ(vxcore::RenameNoReplace(source, moved), VXCORE_OK);
  ASSERT_FALSE(fs::exists(source));
  ASSERT_EQ(read_binary(moved), source_bytes);
  ASSERT_EQ(vxcore::GetFileIdentity(moved, after), VXCORE_OK);
  ASSERT_TRUE(before.SameObject(after));

  // Even an empty existing directory is not a replaceable destination.
  const auto from_dir = base / "from-dir";
  const auto to_dir = base / "to-dir";
  ASSERT_TRUE(fs::create_directory(from_dir));
  ASSERT_TRUE(fs::create_directory(to_dir));
  ASSERT_EQ(vxcore::WriteFileAtomic(from_dir / "keep", source_bytes), VXCORE_OK);
  ASSERT_NE(vxcore::RenameNoReplace(from_dir, to_dir), VXCORE_OK);
  ASSERT_EQ(read_binary(from_dir / "keep"), source_bytes);
  ASSERT_TRUE(fs::is_empty(to_dir));
  ASSERT_EQ(vxcore::RenameNoReplace(from_dir, base / "moved-dir"), VXCORE_OK);
  ASSERT_EQ(read_binary(base / "moved-dir" / "keep"), source_bytes);

  // Both writers selected the same absent name; only the first may publish.
  const auto raced = base / "raced.bin";
  {
    vxcore::AtomicFileWriter first(raced), second(raced);
    ASSERT_EQ(first.Open(), VXCORE_OK);
    ASSERT_EQ(second.Open(), VXCORE_OK);
    ASSERT_EQ(first.Write(winner_bytes.data(), winner_bytes.size()), VXCORE_OK);
    ASSERT_EQ(second.Write(source_bytes.data(), source_bytes.size()), VXCORE_OK);
    ASSERT_FALSE(fs::exists(raced));
    ASSERT_EQ(first.CommitNoReplace(), VXCORE_OK);
    ASSERT_EQ(vxcore::GetFileIdentity(raced, before), VXCORE_OK);
    ASSERT_NE(second.CommitNoReplace(), VXCORE_OK);
    ASSERT_EQ(vxcore::GetFileIdentity(raced, after), VXCORE_OK);
    ASSERT_TRUE(before.SameObject(after));
  }
  ASSERT_EQ(read_binary(raced), winner_bytes);
  {
    vxcore::AtomicFileWriter failed(base / "failed.bin");
    ASSERT_EQ(failed.Open(), VXCORE_OK);
    ASSERT_EQ(failed.Write(source_bytes.data(), source_bytes.size()), VXCORE_OK);
    ASSERT_EQ(failed.Write(nullptr, 1), VXCORE_ERR_NULL_POINTER);
    ASSERT_EQ(failed.CommitNoReplace(), VXCORE_ERR_NULL_POINTER);
  }
  ASSERT_FALSE(fs::exists(base / "failed.bin"));
  {
    vxcore::AtomicFileWriter abandoned(base / "abandoned.bin");
    ASSERT_EQ(abandoned.Open(), VXCORE_OK);
    ASSERT_EQ(abandoned.Write(source_bytes.data(), source_bytes.size()), VXCORE_OK);
  }
  ASSERT_FALSE(fs::exists(base / "abandoned.bin"));
  {
    vxcore::AtomicFileWriter empty(base / "empty.bin");
    ASSERT_EQ(empty.Open(), VXCORE_OK);
    ASSERT_EQ(empty.Write(nullptr, 0), VXCORE_OK);
    ASSERT_EQ(empty.CommitNoReplace(), VXCORE_OK);
  }
  ASSERT_EQ(fs::file_size(base / "empty.bin"), 0);
  for (const auto &entry : fs::directory_iterator(base)) {
    ASSERT_TRUE(vxcore::PathToUtf8(entry.path().filename()).find(".tmp-") == std::string::npos);
  }
  fs::remove_all(base);
  return 0;
}

int test_file_identity_and_directory_flush() {
  namespace fs = std::filesystem;
  const auto base = vxcore::PathFromUtf8(get_test_path("file_identity"));
  fs::remove_all(base);
  ASSERT_TRUE(fs::create_directories(base));
  const auto file = base / "ordinary.bin";
  ASSERT_EQ(vxcore::WriteFileAtomic(file, "bytes"), VXCORE_OK);
  vxcore::FileIdentity file_id, directory_id, changed;
  ASSERT_EQ(vxcore::GetFileIdentity(file, file_id), VXCORE_OK);
  ASSERT_FALSE(file_id.directory);
  ASSERT_TRUE(file_id.device != 0 && (file_id.low != 0 || file_id.high != 0));
  ASSERT_EQ(vxcore::GetFileIdentity(base, directory_id), VXCORE_OK);
  ASSERT_TRUE(directory_id.directory);
  ASSERT_FALSE(file_id.SameObject(directory_id));
  ASSERT_TRUE(fs::create_directory(base / "child"));
  ASSERT_EQ(vxcore::GetFileIdentity(base, changed), VXCORE_OK);
  ASSERT_TRUE(directory_id.SameObject(changed));
  changed.links += 1;
  ASSERT_TRUE(directory_id.SameObject(changed));
  changed.high ^= 1;
  ASSERT_FALSE(directory_id.SameObject(changed));
  changed = {};
  ASSERT_FALSE(changed.SameObject(changed));

  std::error_code ec;
  fs::create_hard_link(file, base / "hard-link", ec);
  if (ec) {
#ifdef __ANDROID__
    ASSERT_TRUE(ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted);
    std::cout << "Hardlink fixture unavailable under Android policy: " << ec.message() << '\n';
#else
    ASSERT_FALSE(ec);
#endif
  } else {
    ASSERT_EQ(vxcore::GetFileIdentity(base / "hard-link", changed), VXCORE_OK);
    ASSERT_TRUE(file_id.SameObject(changed));
    ASSERT_EQ(changed.links, 2);
  }
  ASSERT_EQ(vxcore::FlushDirectory(base), VXCORE_OK);
  ASSERT_NE(vxcore::FlushDirectory(file), VXCORE_OK);
  ASSERT_NE(vxcore::FlushDirectory(base / "missing"), VXCORE_OK);
  changed = file_id;
  ASSERT_NE(vxcore::GetFileIdentity(base / "missing", changed), VXCORE_OK);
  ASSERT_EQ(changed.device, 0);
  ASSERT_EQ(changed.low, 0);
  ASSERT_EQ(changed.high, 0);
  ASSERT_EQ(changed.links, 0);
  ASSERT_FALSE(changed.directory);

  const auto link = base / "directory-link";
  ASSERT_TRUE(vxcore_test::create_junction(vxcore::PathToUtf8(base / "child"),
                                           vxcore::PathToUtf8(link)));
  ASSERT_NE(vxcore::GetFileIdentity(link, changed), VXCORE_OK);
  ASSERT_NE(vxcore::GetFileIdentity(link / "", changed), VXCORE_OK);
  ASSERT_FALSE(changed.SameObject(changed));
  ASSERT_NE(vxcore::FlushDirectory(link), VXCORE_OK);
  ASSERT_NE(vxcore::FlushDirectory(link / ""), VXCORE_OK);
  vxcore_test::remove_reparse_dir(vxcore::PathToUtf8(link));
#ifndef _WIN32
  const int fifo_result = ::mkfifo((base / "fifo").c_str(), 0600);
#ifdef __ANDROID__
  if (fifo_result != 0 && (errno == EACCES || errno == EPERM)) {
    std::cout << "FIFO fixture unavailable under Android policy" << '\n';
  } else
#endif
  {
    ASSERT_EQ(fifo_result, 0);
    changed = file_id;
    ASSERT_NE(vxcore::GetFileIdentity(base / "fifo", changed), VXCORE_OK);
    ASSERT_EQ(changed.device, 0);
    ASSERT_EQ(changed.links, 0);
  }
#endif
  fs::remove_all(base);
  return 0;
}

int test_no_replace_postpublication_flush_error() {
  namespace fs = std::filesystem;
  const auto base = vxcore::PathFromUtf8(get_test_path("no_replace_flush_failure"));
  fs::remove_all(base);
  ASSERT_TRUE(fs::create_directories(base / "actual"));
  const auto alias = base / "alias";
  ASSERT_TRUE(vxcore_test::create_junction(vxcore::PathToUtf8(base / "actual"),
                                           vxcore::PathToUtf8(alias)));
  const std::string bytes("complete\0\xff\r\n", 12);
  {
    // The byte publication succeeds through this parent alias, but the
    // directory flush rejects the final reparse/symlink. No fault hook is needed.
    vxcore::AtomicFileWriter writer(alias / "published.bin");
    ASSERT_EQ(writer.Open(), VXCORE_OK);
    ASSERT_EQ(writer.Write(bytes.data(), bytes.size()), VXCORE_OK);
    ASSERT_NE(writer.CommitNoReplace(), VXCORE_OK);
    ASSERT_TRUE(fs::exists(base / "actual" / "published.bin"));
  }
  std::ifstream input(base / "actual" / "published.bin", std::ios::binary);
  const std::string actual((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  input.close();
  ASSERT_EQ(actual, bytes);
  ASSERT_EQ(std::distance(fs::directory_iterator(base / "actual"), fs::directory_iterator()), 1);
  vxcore_test::remove_reparse_dir(vxcore::PathToUtf8(alias));
  fs::remove_all(base);
  return 0;
}

namespace {
// An exception must identify the operation, never disclose a private path.
template <typename Operation>
bool PrivateOperationRefused(Operation operation) {
  try {
    operation();
  } catch (const std::filesystem::filesystem_error &error) {
    return error.path1().empty() && error.path2().empty() &&
           std::string(error.what()).find("Private filesystem operation failed") != std::string::npos;
  }
  return false;
}

std::string ReadPrivateUtilityFile(const std::filesystem::path &path) {
  vxcore::private_io::Validate(path);
  std::ifstream input;
  {
    const vxcore::NativePathArgument argument(path, vxcore::FileAccess::kPrivate);
    input.open(argument.c_str(), std::ios::binary);
  }
  if (!input) throw std::runtime_error("Private test read failed");
  std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (input.bad()) throw std::runtime_error("Private test read failed");
  return bytes;
}
}  // namespace

int test_private_native_path_arguments() {
  namespace fs = std::filesystem;
  using vxcore::FileAccess;
  using vxcore::NativePathArgument;
  const fs::path ordinary = vxcore::PathFromUtf8("relative/../ordinary.txt");
  const NativePathArgument default_argument(ordinary);
  ASSERT_TRUE(default_argument.c_str() == ordinary.c_str());

#ifdef _WIN32
  const fs::path dos = L"c:/private/\u6d4b\u8bd5/file.bin";
  const auto original = dos.native();
  const NativePathArgument dos_argument(dos, FileAccess::kPrivate);
  ASSERT_TRUE(std::wstring(dos_argument.c_str()) == L"\\\\?\\c:\\private\\\u6d4b\u8bd5\\file.bin");
  ASSERT_TRUE(dos.native() == original);
  const fs::path backslashes = L"C:\\private\\file.bin";
  const NativePathArgument backslash_argument(backslashes, FileAccess::kPrivate);
  ASSERT_TRUE(std::wstring(backslash_argument.c_str()) == L"\\\\?\\C:\\private\\file.bin");
  const fs::path drive_root = L"C:/";
  ASSERT_TRUE(std::wstring(NativePathArgument(drive_root, FileAccess::kPrivate).c_str()) == L"\\\\?\\C:\\");

  // Spelling-only UNC coverage: these do not claim access to a live share.
  const fs::path unc = L"//server/share/private/\u6d4b\u8bd5.bin";
  const NativePathArgument unc_argument(unc, FileAccess::kPrivate);
  ASSERT_TRUE(std::wstring(unc_argument.c_str()) == L"\\\\?\\UNC\\server\\share\\private\\\u6d4b\u8bd5.bin");
  const fs::path unc_backslashes = L"\\\\server\\share\\private\\file.bin";
  ASSERT_TRUE(std::wstring(NativePathArgument(unc_backslashes, FileAccess::kPrivate).c_str()) ==
              L"\\\\?\\UNC\\server\\share\\private\\file.bin");
  const fs::path unc_root = L"//server/share";
  ASSERT_TRUE(std::wstring(NativePathArgument(unc_root, FileAccess::kPrivate).c_str()) ==
              L"\\\\?\\UNC\\server\\share");
  const std::vector<std::wstring> invalid = {
      L"", L"relative", L"C:relative", L"/private/file", L"\\private\\file",
      L"C://private", L"C:/private//file", L"C:/private/./file", L"C:/private/../file",
      L"C:/private/file.", L"C:/private/file ", L"C:/private/file:stream", L"C:/private/fi*le",
      L"C:/private/fi?le", L"C:/private/fi<le", L"C:/private/fi>le", L"C:/private/fi|le",
      L"C:/private/fi\"le", L"C:/private/fi\x1fle", L"C:/private/NUL.txt", L"C:/private/CON .txt",
      L"C:/private/conin$", L"C:/private/CONOUT$", L"C:/private/COM1", L"C:/private/LPT9.txt",
      L"C:/private/COM\u00b9", L"C:/private/LPT\u00b2.txt", L"C:/private/LPT\u00b3",
      L"//", L"//server", L"//server/", L"///share/file", L"//server//file",
      L"//./share/file", L"//server/../file", L"//server/share//file", L"//server/share/./file",
      L"//server/share/../file", L"//server/share./file", L"//server/share /file",
      L"//server/share:stream/file", L"\\\\?\\C:\\private", L"\\\\?\\UNC\\server\\share\\file",
      L"\\\\.\\C:\\private", L"\\??\\C:\\private", L"//?/C:/private", L"//./pipe/name"};
  for (const auto &value : invalid) {
    const fs::path path(value);
    ASSERT_TRUE(PrivateOperationRefused([&] { NativePathArgument argument(path, FileAccess::kPrivate); }));
    ASSERT_TRUE(PrivateOperationRefused([&] { vxcore::private_io::Validate(path, true); }));
  }
  std::wstring nul = L"C:/private/secret";
  nul.push_back(L'\0');
  nul += L"hidden";
  const fs::path nul_path(nul);
  ASSERT_TRUE(PrivateOperationRefused([&] { NativePathArgument argument(nul_path, FileAccess::kPrivate); }));
  std::wstring surrogate = L"C:/private/";
  surrogate.push_back(static_cast<wchar_t>(0xd800));
  const fs::path surrogate_path(surrogate);
  ASSERT_TRUE(PrivateOperationRefused([&] { NativePathArgument argument(surrogate_path, FileAccess::kPrivate); }));
#else
  const fs::path absolute = "/private/ordinary.bin";
  const NativePathArgument private_argument(absolute, FileAccess::kPrivate);
  ASSERT_TRUE(private_argument.c_str() == absolute.c_str());
  const std::vector<std::string> invalid = {"", "relative", "//private", "/private//file",
                                          "/private/./file", "/private/../file", "/private/file:stream"};
  for (const auto &value : invalid) {
    const fs::path path(value);
    ASSERT_TRUE(PrivateOperationRefused([&] { NativePathArgument argument(path, FileAccess::kPrivate); }));
    ASSERT_TRUE(PrivateOperationRefused([&] { vxcore::private_io::Validate(path, true); }));
  }
  const fs::path nul_path(std::string("/private/file\0hidden", 20));
  ASSERT_TRUE(PrivateOperationRefused([&] { NativePathArgument argument(nul_path, FileAccess::kPrivate); }));
#endif
  ASSERT_TRUE(PrivateOperationRefused([&] { NativePathArgument argument(ordinary, FileAccess::kPrivate); }));
  vxcore::FileIdentity identity;
  ASSERT_EQ(vxcore::GetFileIdentity(ordinary, identity, FileAccess::kPrivate), VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore::WriteFileAtomic(ordinary, "not written", FileAccess::kPrivate), VXCORE_ERR_INVALID_PARAM);
  const std::string embedded_nul("private\0hidden", 14);
  ASSERT_TRUE(vxcore::CheckReparsePoint(embedded_nul, FileAccess::kPrivate) == vxcore::ReparseState::kError);
  int64_t created = 17, modified = 19;
  ASSERT_FALSE(vxcore::GetFilesystemTimes(embedded_nul, &created, &modified, FileAccess::kPrivate));
  ASSERT_EQ(created, 17);
  ASSERT_EQ(modified, 19);
  return 0;
}

int test_private_long_path_utilities() {
  namespace fs = std::filesystem;
  namespace io = vxcore::private_io;
  constexpr auto access = vxcore::FileAccess::kPrivate;
  // Resolve only the test's ambient temp root, before selecting private access.
  // Every private field below stays logical and every private I/O opts in.
  const auto base = fs::canonical(fs::temp_directory_path()) /
                    ("vxcore_private_" + std::to_string(
                        std::chrono::steady_clock::now().time_since_epoch().count()));
  ASSERT_TRUE(io::MakeDirectory(base));
  std::vector<fs::path> directories{base};
  auto directory = base;
  while (directory.native().size() < 290) {
    directory /= "private_component_0123456789";
    directories.push_back(directory);
  }
  io::CreateDirectories(directory);
  io::CreateDirectories(directory);
  ASSERT_TRUE(directory.native().size() >= 260);
  ASSERT_FALSE(io::MakeDirectory(directory));
  ASSERT_TRUE(io::Status(directory).directory);
  ASSERT_TRUE(io::IsDirectory(directory));
  ASSERT_FALSE(io::IsRegularFile(directory));
  ASSERT_TRUE(io::IsEmpty(directory));
  io::Validate(directory);
  const auto missing = directory / "missing" / "nested";
  ASSERT_FALSE(io::Exists(missing));
  ASSERT_FALSE(io::IsDirectory(missing));
  ASSERT_FALSE(io::IsRegularFile(missing));
  ASSERT_FALSE(io::Remove(missing));
  io::Validate(missing, true);
  ASSERT_TRUE(PrivateOperationRefused([&] { io::Validate(missing); }));
  ASSERT_TRUE(PrivateOperationRefused([&] { io::FileSize(missing); }));
  ASSERT_TRUE(PrivateOperationRefused([&] { io::ModifiedTime(missing); }));
  ASSERT_TRUE(PrivateOperationRefused([&] { io::IsEmpty(missing); }));
  ASSERT_TRUE(PrivateOperationRefused([&] { io::MakeDirectory(missing); }));

  const auto destination = directory / vxcore::PathFromUtf8(u8"快照.bin");
  const std::string original("original\0\xff\r\n", 12);
  const std::string replacement("replacement\0\x80\n", 14);
  ASSERT_EQ(vxcore::WriteFileAtomic(destination, original, access), VXCORE_OK);
  ASSERT_EQ(ReadPrivateUtilityFile(destination), original);
  auto status = io::Status(destination);
  ASSERT_TRUE(status.exists && status.regular && !status.directory);
  ASSERT_EQ(status.size, original.size());
  ASSERT_EQ(io::FileSize(destination), original.size());
  ASSERT_EQ(io::ModifiedTime(destination), status.modified);
  ASSERT_FALSE(io::IsEmpty(destination));
  ASSERT_FALSE(io::IsEmpty(directory));
  ASSERT_TRUE(vxcore::CheckReparsePoint(vxcore::PathToUtf8(destination), access) == vxcore::ReparseState::kNo);
  ASSERT_TRUE(vxcore::CheckReparsePoint(vxcore::PathToUtf8(missing), access) == vxcore::ReparseState::kError);
  int64_t created = 0, modified = 0;
  ASSERT_TRUE(vxcore::GetFilesystemTimes(vxcore::PathToUtf8(destination), &created, &modified, access));
  ASSERT_TRUE(created > 0 && modified > 0);
  vxcore::FileIdentity before, after;
  ASSERT_EQ(vxcore::GetFileIdentity(destination, before, access), VXCORE_OK);
  ASSERT_FALSE(before.directory);
  ASSERT_EQ(vxcore::FlushDirectory(directory, access), VXCORE_OK);
  ASSERT_NE(vxcore::FlushDirectory(destination, access), VXCORE_OK);
  ASSERT_TRUE(PrivateOperationRefused([&] { io::Status(destination / "child"); }));
  ASSERT_TRUE(PrivateOperationRefused([&] { io::CreateDirectories(destination / "child"); }));
  ASSERT_TRUE(PrivateOperationRefused([&] { io::FileSize(directory); }));
  ASSERT_TRUE(PrivateOperationRefused([&] { io::Remove(directory); }));

  {
    vxcore::AtomicFileWriter writer(destination, access);
    ASSERT_EQ(writer.Open(), VXCORE_OK);
    ASSERT_EQ(writer.Write(replacement.data(), replacement.size()), VXCORE_OK);
    ASSERT_EQ(ReadPrivateUtilityFile(destination), original);
    uint64_t staged_bytes = 0;
    size_t staged_files = 0;
    io::VisitDirectory(directory, [&](const fs::path &entry) {
      ++staged_files;
      staged_bytes += io::FileSize(entry);
    });
    ASSERT_EQ(staged_files, size_t{2});
    // Buffered bytes may not yet reach disk; reserve the full atomic sibling.
    ASSERT_TRUE(staged_bytes >= original.size());
    ASSERT_TRUE(staged_bytes <= original.size() + replacement.size());
    ASSERT_EQ(vxcore::GetFileIdentity(destination, after, access), VXCORE_OK);
    ASSERT_TRUE(before.SameObject(after));
    ASSERT_EQ(writer.Commit(), VXCORE_OK);
  }
  ASSERT_EQ(ReadPrivateUtilityFile(destination), replacement);
  ASSERT_EQ(vxcore::GetFileIdentity(destination, after, access), VXCORE_OK);
  ASSERT_FALSE(before.SameObject(after));
  {
    vxcore::AtomicFileWriter abandoned(destination, access);
    ASSERT_EQ(abandoned.Open(), VXCORE_OK);
    ASSERT_EQ(abandoned.Write(original.data(), original.size()), VXCORE_OK);
  }
  {
    vxcore::AtomicFileWriter failed(destination, access);
    ASSERT_EQ(failed.Open(), VXCORE_OK);
    ASSERT_EQ(failed.Write(nullptr, 1), VXCORE_ERR_NULL_POINTER);
    ASSERT_EQ(failed.Commit(), VXCORE_ERR_NULL_POINTER);
  }
  ASSERT_EQ(ReadPrivateUtilityFile(destination), replacement);
  size_t count = 0;
  bool logical_names = true;
  io::VisitDirectory(directory, [&](const fs::path &child) {
    ++count;
    logical_names = logical_names && child == destination && child.parent_path() == directory;
  });
  ASSERT_EQ(count, 1);
  ASSERT_TRUE(logical_names);
  const auto winner = directory / "winner.bin";
  {
    vxcore::AtomicFileWriter first(winner, access), second(winner, access);
    ASSERT_EQ(first.Open(), VXCORE_OK);
    ASSERT_EQ(second.Open(), VXCORE_OK);
    ASSERT_EQ(first.Write(original.data(), original.size()), VXCORE_OK);
    ASSERT_EQ(second.Write(replacement.data(), replacement.size()), VXCORE_OK);
    ASSERT_EQ(first.CommitNoReplace(), VXCORE_OK);
    ASSERT_NE(second.CommitNoReplace(), VXCORE_OK);
  }
  ASSERT_EQ(ReadPrivateUtilityFile(winner), original);
  ASSERT_NE(vxcore::RenameNoReplace(destination, winner, access), VXCORE_OK);
  ASSERT_EQ(ReadPrivateUtilityFile(destination), replacement);
  const auto moved = directory / "moved.bin";
  ASSERT_EQ(vxcore::GetFileIdentity(destination, before, access), VXCORE_OK);
  ASSERT_EQ(vxcore::RenameNoReplace(destination, moved, access), VXCORE_OK);
  ASSERT_FALSE(io::Exists(destination));
  ASSERT_EQ(vxcore::GetFileIdentity(moved, after, access), VXCORE_OK);
  ASSERT_TRUE(before.SameObject(after));
  ASSERT_EQ(ReadPrivateUtilityFile(moved), replacement);
  ASSERT_EQ(vxcore::WriteFileAtomic(moved, "", access), VXCORE_OK);
  ASSERT_TRUE(io::IsEmpty(moved));

  const auto child_directory = directory / "child";
  ASSERT_TRUE(io::MakeDirectory(child_directory));
  ASSERT_FALSE(io::MakeDirectory(child_directory));
  ASSERT_EQ(vxcore::GetFileIdentity(child_directory, after, access), VXCORE_OK);
  ASSERT_TRUE(after.directory);
  ASSERT_EQ(vxcore::FlushDirectory(child_directory, access), VXCORE_OK);
  const auto renamed_directory = directory / "renamed-child";
  ASSERT_EQ(vxcore::RenameNoReplace(child_directory, renamed_directory, access), VXCORE_OK);
  ASSERT_EQ(vxcore::GetFileIdentity(renamed_directory, before, access), VXCORE_OK);
  ASSERT_TRUE(before.SameObject(after));

#ifdef _WIN32
  // Denying sharing prevents deletion/replacement, not FILE_READ_ATTRIBUTES
  // metadata probes. Exercise the actual denied mutation without broadening the
  // production probe's access mask just to manufacture a status failure.
  ASSERT_EQ(vxcore::GetFileIdentity(winner, before, access), VXCORE_OK);
  HANDLE exclusive;
  {
    const vxcore::NativePathArgument argument(winner, access);
    exclusive = CreateFileW(argument.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  }
  ASSERT_TRUE(exclusive != INVALID_HANDLE_VALUE);
  const bool refused = PrivateOperationRefused([&] { io::Remove(winner); });
  const auto replacement_error = vxcore::WriteFileAtomic(winner, replacement, access);
  ASSERT_TRUE(CloseHandle(exclusive) != 0);
  ASSERT_TRUE(refused);
  ASSERT_EQ(replacement_error, VXCORE_ERR_IO);
  ASSERT_EQ(vxcore::GetFileIdentity(winner, after, access), VXCORE_OK);
  ASSERT_TRUE(before.SameObject(after));
  ASSERT_EQ(ReadPrivateUtilityFile(winner), original);
#endif

  // Install a real junction/symlink at the long leaf without asking the private
  // mutation API to authorize it. The short fixture helper needs no Windows privilege.
  const auto target = base / "target";
  ASSERT_TRUE(io::MakeDirectory(target));
  ASSERT_EQ(vxcore::WriteFileAtomic(target / "keep", "untouched", access), VXCORE_OK);
  const auto short_link = base / "fixture-link";
  const auto long_link = directory / "linked";
  ASSERT_TRUE(vxcore_test::create_junction(vxcore::PathToUtf8(target), vxcore::PathToUtf8(short_link)));
  {
    const vxcore::NativePathArgument source(short_link, access), destination_argument(long_link, access);
#ifdef _WIN32
    ASSERT_TRUE(MoveFileExW(source.c_str(), destination_argument.c_str(), MOVEFILE_WRITE_THROUGH) != 0);
#else
    ASSERT_EQ(::rename(source.c_str(), destination_argument.c_str()), 0);
#endif
  }
  ASSERT_TRUE(vxcore::CheckReparsePoint(vxcore::PathToUtf8(long_link), access) == vxcore::ReparseState::kYes);
  ASSERT_TRUE(PrivateOperationRefused([&] { io::Status(long_link); }));
  ASSERT_TRUE(PrivateOperationRefused([&] { io::Exists(long_link / "keep"); }));
  ASSERT_TRUE(PrivateOperationRefused([&] { io::Status(long_link / "missing"); }));
  ASSERT_TRUE(PrivateOperationRefused([&] { io::Validate(long_link / "missing", true); }));
  ASSERT_TRUE(PrivateOperationRefused([&] { io::CreateDirectories(long_link / "new"); }));
  ASSERT_TRUE(PrivateOperationRefused([&] { io::MakeDirectory(long_link); }));
  ASSERT_TRUE(PrivateOperationRefused([&] { io::Remove(long_link); }));
  bool visited_link = false;
  ASSERT_TRUE(PrivateOperationRefused([&] {
    io::VisitDirectory(directory, [&](const fs::path &child) { visited_link |= child == long_link; });
  }));
  ASSERT_FALSE(visited_link);
  ASSERT_TRUE(PrivateOperationRefused([&] { io::IsEmpty(directory); }));
  ASSERT_NE(vxcore::GetFileIdentity(long_link, after, access), VXCORE_OK);
  ASSERT_NE(vxcore::GetFileIdentity(long_link / "keep", after, access), VXCORE_OK);
  ASSERT_NE(vxcore::FlushDirectory(long_link, access), VXCORE_OK);
  ASSERT_FALSE(vxcore::GetFilesystemTimes(vxcore::PathToUtf8(long_link / "keep"), &created, &modified, access));
  ASSERT_NE(vxcore::WriteFileAtomic(long_link / "keep", "wrong", access), VXCORE_OK);
  ASSERT_NE(vxcore::RenameNoReplace(winner, long_link / "new", access), VXCORE_OK);
  ASSERT_EQ(ReadPrivateUtilityFile(target / "keep"), "untouched");
  {
    // Remove only the adversarial link installed by this fixture, never its target.
    const vxcore::NativePathArgument argument(long_link, access);
#ifdef _WIN32
    ASSERT_TRUE(RemoveDirectoryW(argument.c_str()) != 0);
#else
    ASSERT_EQ(::unlink(argument.c_str()), 0);
#endif
  }
#ifndef _WIN32
  const auto fifo = directory / "fifo";
  const vxcore::NativePathArgument fifo_argument(fifo, access);
  const int fifo_result = ::mkfifo(fifo_argument.c_str(), 0600);
#ifdef __ANDROID__
  if (fifo_result != 0 && (errno == EACCES || errno == EPERM)) {
    std::cout << "FIFO fixture unavailable under Android policy" << '\n';
  } else
#endif
  {
    ASSERT_EQ(fifo_result, 0);
    ASSERT_TRUE(PrivateOperationRefused([&] { io::Status(fifo); }));
    ASSERT_TRUE(PrivateOperationRefused([&] { io::Remove(fifo); }));
    ASSERT_TRUE(PrivateOperationRefused([&] { io::VisitDirectory(directory, [](const fs::path &) {}); }));
    ASSERT_EQ(::unlink(fifo_argument.c_str()), 0);
  }
#endif
  ASSERT_TRUE(io::Remove(target / "keep"));
  ASSERT_TRUE(io::Remove(target));
  ASSERT_TRUE(io::Remove(renamed_directory));
  ASSERT_TRUE(io::Remove(winner));
  ASSERT_TRUE(io::Remove(moved));
  ASSERT_TRUE(io::IsEmpty(directory));
  for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
    ASSERT_TRUE(io::Remove(*it));
  }
  ASSERT_FALSE(io::Exists(base));
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
  RUN_TEST(test_atomic_file_publication);
  RUN_TEST(test_no_replace_publication);
  RUN_TEST(test_file_identity_and_directory_flush);
  RUN_TEST(test_no_replace_postpublication_flush_error);
  RUN_TEST(test_private_native_path_arguments);
  RUN_TEST(test_private_long_path_utilities);

  std::cout << "✓ All file_utils tests passed" << std::endl;
  return 0;
}
