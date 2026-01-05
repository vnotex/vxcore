#include <filesystem>
#include <iostream>
#include <string>

#include "search/search_file_info.h"
#include "search/search_query.h"
#include "search/simple_search_backend.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "utils/string_utils.h"

using namespace vxcore;

class SimpleSearchBackendTest {
 public:
  static bool MatchesPattern(SimpleSearchBackend &backend, const std::string &line,
                             const std::string &pattern, SearchOption options,
                             std::vector<SearchMatch> &out_matches) {
    return backend.MatchesPattern(line, pattern, options, out_matches);
  }
};

int test_to_lower() {
  std::cout << "  Running test_to_lower..." << std::endl;

  ASSERT_EQ(ToLowerString("HELLO"), "hello");
  ASSERT_EQ(ToLowerString("MixedCase"), "mixedcase");
  ASSERT_EQ(ToLowerString("already_lowercase"), "already_lowercase");
  ASSERT_EQ(ToLowerString("123ABC"), "123abc");

  std::cout << "  ✓ test_to_lower passed" << std::endl;
  return 0;
}

int test_matches_pattern_case_sensitive() {
  std::cout << "  Running test_matches_pattern_case_sensitive..." << std::endl;

  SimpleSearchBackend backend;
  std::vector<SearchMatch> matches;

  ASSERT_TRUE(SimpleSearchBackendTest::MatchesPattern(backend, "hello world", "hello",
                                                      SearchOption::kCaseSensitive, matches));
  ASSERT_EQ(matches.size(), 1);
  ASSERT_EQ(matches[0].column_start, 0);
  ASSERT_EQ(matches[0].column_end, 5);

  matches.clear();
  ASSERT_FALSE(SimpleSearchBackendTest::MatchesPattern(backend, "hello world", "HELLO",
                                                       SearchOption::kCaseSensitive, matches));
  ASSERT_EQ(matches.size(), 0);

  std::cout << "  ✓ test_matches_pattern_case_sensitive passed" << std::endl;
  return 0;
}

int test_matches_pattern_case_insensitive() {
  std::cout << "  Running test_matches_pattern_case_insensitive..." << std::endl;

  SimpleSearchBackend backend;
  std::vector<SearchMatch> matches;

  ASSERT_TRUE(SimpleSearchBackendTest::MatchesPattern(backend, "hello world", "HELLO",
                                                      SearchOption::kNone, matches));
  ASSERT_EQ(matches.size(), 1);
  ASSERT_EQ(matches[0].column_start, 0);
  ASSERT_EQ(matches[0].column_end, 5);

  std::cout << "  ✓ test_matches_pattern_case_insensitive passed" << std::endl;
  return 0;
}

int test_matches_pattern_whole_word() {
  std::cout << "  Running test_matches_pattern_whole_word..." << std::endl;

  SimpleSearchBackend backend;
  std::vector<SearchMatch> matches;

  ASSERT_TRUE(SimpleSearchBackendTest::MatchesPattern(
      backend, "hello world", "hello", SearchOption::kCaseSensitive | SearchOption::kWholeWord,
      matches));
  ASSERT_EQ(matches.size(), 1);

  matches.clear();
  ASSERT_FALSE(SimpleSearchBackendTest::MatchesPattern(
      backend, "helloworld", "hello", SearchOption::kCaseSensitive | SearchOption::kWholeWord,
      matches));
  ASSERT_EQ(matches.size(), 0);

  matches.clear();
  ASSERT_TRUE(SimpleSearchBackendTest::MatchesPattern(
      backend, "test hello test", "hello", SearchOption::kCaseSensitive | SearchOption::kWholeWord,
      matches));
  ASSERT_EQ(matches.size(), 1);
  ASSERT_EQ(matches[0].column_start, 5);

  std::cout << "  ✓ test_matches_pattern_whole_word passed" << std::endl;
  return 0;
}

int test_matches_pattern_regex() {
  std::cout << "  Running test_matches_pattern_regex..." << std::endl;

  SimpleSearchBackend backend;
  std::vector<SearchMatch> matches;

  ASSERT_TRUE(SimpleSearchBackendTest::MatchesPattern(
      backend, "test123", "test\\d+", SearchOption::kCaseSensitive | SearchOption::kRegex,
      matches));
  ASSERT_EQ(matches.size(), 1);

  matches.clear();
  ASSERT_TRUE(SimpleSearchBackendTest::MatchesPattern(
      backend, "hello world", "h.*?o", SearchOption::kCaseSensitive | SearchOption::kRegex,
      matches));
  ASSERT_EQ(matches.size(), 1);

  std::cout << "  ✓ test_matches_pattern_regex passed" << std::endl;
  return 0;
}

int test_matches_pattern_regex_case_insensitive() {
  std::cout << "  Running test_matches_pattern_regex_case_insensitive..." << std::endl;

  SimpleSearchBackend backend;
  std::vector<SearchMatch> matches;

  ASSERT_TRUE(SimpleSearchBackendTest::MatchesPattern(backend, "HELLO world", "h.*o",
                                                      SearchOption::kRegex, matches));
  ASSERT_EQ(matches.size(), 1);

  std::cout << "  ✓ test_matches_pattern_regex_case_insensitive passed" << std::endl;
  return 0;
}

int test_matches_pattern_multiple_matches() {
  std::cout << "  Running test_matches_pattern_multiple_matches..." << std::endl;

  SimpleSearchBackend backend;
  std::vector<SearchMatch> matches;

  ASSERT_TRUE(SimpleSearchBackendTest::MatchesPattern(backend, "test test test", "test",
                                                      SearchOption::kCaseSensitive, matches));
  ASSERT_EQ(matches.size(), 3);
  ASSERT_EQ(matches[0].column_start, 0);
  ASSERT_EQ(matches[1].column_start, 5);
  ASSERT_EQ(matches[2].column_start, 10);

  std::cout << "  ✓ test_matches_pattern_multiple_matches passed" << std::endl;
  return 0;
}

int test_matches_pattern_utf8() {
  std::cout << "  Running test_matches_pattern_utf8..." << std::endl;

  SimpleSearchBackend backend;
  std::vector<SearchMatch> matches;

  ASSERT_TRUE(SimpleSearchBackendTest::MatchesPattern(backend, "你好世界", "你好",
                                                      SearchOption::kCaseSensitive, matches));
  ASSERT_EQ(matches.size(), 1);

  std::cout << "  ✓ test_matches_pattern_utf8 passed" << std::endl;
  return 0;
}

int test_matches_pattern_invalid_regex() {
  std::cout << "  Running test_matches_pattern_invalid_regex..." << std::endl;

  SimpleSearchBackend backend;
  std::vector<SearchMatch> matches;

  ASSERT_FALSE(SimpleSearchBackendTest::MatchesPattern(
      backend, "test", "[invalid", SearchOption::kCaseSensitive | SearchOption::kRegex, matches));
  ASSERT_EQ(matches.size(), 0);

  std::cout << "  ✓ test_matches_pattern_invalid_regex passed" << std::endl;
  return 0;
}

int test_search_single_file() {
  std::cout << "  Running test_search_single_file..." << std::endl;

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test1.txt");
  write_file(test_file, "hello world\ntest content\nhello again");

  SimpleSearchBackend backend;
  std::vector<SearchFileInfo> files;
  SearchFileInfo file_info;
  file_info.path = "test1.txt";
  file_info.absolute_path = test_file;
  file_info.is_folder = false;
  files.push_back(file_info);

  ContentSearchResult result;
  auto err = backend.Search(files, "hello", SearchOption::kCaseSensitive, {}, 100, result);

  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(result.matched_files.size(), 1);
  ASSERT_EQ(result.matched_files[0].path, "test1.txt");
  ASSERT_EQ(result.matched_files[0].matches.size(), 2);
  ASSERT_EQ(result.matched_files[0].matches[0].line_number, 1);
  ASSERT_EQ(result.matched_files[0].matches[1].line_number, 3);
  ASSERT_FALSE(result.truncated);

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_search_single_file passed" << std::endl;
  return 0;
}

int test_search_multiple_files() {
  std::cout << "  Running test_search_multiple_files..." << std::endl;

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file1 = CleanPath(test_dir + "/file1.txt");
  std::string test_file2 = CleanPath(test_dir + "/file2.txt");
  write_file(test_file1, "hello world\ntest content");
  write_file(test_file2, "another test\nhello there");

  SimpleSearchBackend backend;
  std::vector<SearchFileInfo> files;

  SearchFileInfo file_info1;
  file_info1.path = "file1.txt";
  file_info1.absolute_path = test_file1;
  file_info1.is_folder = false;
  files.push_back(file_info1);

  SearchFileInfo file_info2;
  file_info2.path = "file2.txt";
  file_info2.absolute_path = test_file2;
  file_info2.is_folder = false;
  files.push_back(file_info2);

  ContentSearchResult result;
  auto err = backend.Search(files, "test", SearchOption::kCaseSensitive, {}, 100, result);

  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(result.matched_files.size(), 2);
  ASSERT_FALSE(result.truncated);

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_search_multiple_files passed" << std::endl;
  return 0;
}

int test_search_case_insensitive() {
  std::cout << "  Running test_search_case_insensitive..." << std::endl;

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test.txt");
  write_file(test_file, "Hello World\nHELLO world\nhello WORLD");

  SimpleSearchBackend backend;
  std::vector<SearchFileInfo> files;
  SearchFileInfo file_info;
  file_info.path = "test.txt";
  file_info.absolute_path = test_file;
  file_info.is_folder = false;
  files.push_back(file_info);

  ContentSearchResult result;
  auto err = backend.Search(files, "hello", SearchOption::kNone, {}, 100, result);

  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(result.matched_files.size(), 1);
  ASSERT_EQ(result.matched_files[0].matches.size(), 3);

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_search_case_insensitive passed" << std::endl;
  return 0;
}

int test_search_whole_word() {
  std::cout << "  Running test_search_whole_word..." << std::endl;

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test.txt");
  write_file(test_file, "hello world\nhelloworld\ntest hello test");

  SimpleSearchBackend backend;
  std::vector<SearchFileInfo> files;
  SearchFileInfo file_info;
  file_info.path = "test.txt";
  file_info.absolute_path = test_file;
  file_info.is_folder = false;
  files.push_back(file_info);

  ContentSearchResult result;
  auto err = backend.Search(files, "hello", SearchOption::kCaseSensitive | SearchOption::kWholeWord,
                            {}, 100, result);

  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(result.matched_files.size(), 1);
  ASSERT_EQ(result.matched_files[0].matches.size(), 2);
  ASSERT_EQ(result.matched_files[0].matches[0].line_number, 1);
  ASSERT_EQ(result.matched_files[0].matches[1].line_number, 3);

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_search_whole_word passed" << std::endl;
  return 0;
}

int test_search_regex() {
  std::cout << "  Running test_search_regex..." << std::endl;

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test.txt");
  write_file(test_file, "test123\ntest456\nhello\ntest");

  SimpleSearchBackend backend;
  std::vector<SearchFileInfo> files;
  SearchFileInfo file_info;
  file_info.path = "test.txt";
  file_info.absolute_path = test_file;
  file_info.is_folder = false;
  files.push_back(file_info);

  ContentSearchResult result;
  auto err = backend.Search(files, "test\\d+", SearchOption::kCaseSensitive | SearchOption::kRegex,
                            {}, 100, result);

  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(result.matched_files.size(), 1);
  ASSERT_EQ(result.matched_files[0].matches.size(), 2);
  ASSERT_EQ(result.matched_files[0].matches[0].line_number, 1);
  ASSERT_EQ(result.matched_files[0].matches[1].line_number, 2);

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_search_regex passed" << std::endl;
  return 0;
}

int test_search_max_results() {
  std::cout << "  Running test_search_max_results..." << std::endl;

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test.txt");
  write_file(test_file, "test\ntest\ntest\ntest\ntest");

  SimpleSearchBackend backend;
  std::vector<SearchFileInfo> files;
  SearchFileInfo file_info;
  file_info.path = "test.txt";
  file_info.absolute_path = test_file;
  file_info.is_folder = false;
  files.push_back(file_info);

  ContentSearchResult result;
  auto err = backend.Search(files, "test", SearchOption::kCaseSensitive, {}, 3, result);

  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(result.matched_files.size(), 1);
  ASSERT_EQ(result.matched_files[0].matches.size(), 3);
  ASSERT_TRUE(result.truncated);

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_search_max_results passed" << std::endl;
  return 0;
}

int test_search_no_matches() {
  std::cout << "  Running test_search_no_matches..." << std::endl;

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test.txt");
  write_file(test_file, "hello world\ntest content");

  SimpleSearchBackend backend;
  std::vector<SearchFileInfo> files;
  SearchFileInfo file_info;
  file_info.path = "test.txt";
  file_info.absolute_path = test_file;
  file_info.is_folder = false;
  files.push_back(file_info);

  ContentSearchResult result;
  auto err = backend.Search(files, "notfound", SearchOption::kCaseSensitive, {}, 100, result);

  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(result.matched_files.size(), 0);
  ASSERT_FALSE(result.truncated);

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_search_no_matches passed" << std::endl;
  return 0;
}

int test_search_utf8_content() {
  std::cout << "  Running test_search_utf8_content..." << std::endl;

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test.txt");
  write_file(test_file, "你好世界\nこんにちは\n你好朋友");

  SimpleSearchBackend backend;
  std::vector<SearchFileInfo> files;
  SearchFileInfo file_info;
  file_info.path = "test.txt";
  file_info.absolute_path = test_file;
  file_info.is_folder = false;
  files.push_back(file_info);

  ContentSearchResult result;
  auto err = backend.Search(files, "你好", SearchOption::kCaseSensitive, {}, 100, result);

  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(result.matched_files.size(), 1);
  ASSERT_EQ(result.matched_files[0].matches.size(), 2);

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_search_utf8_content passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running simple_search_backend tests..." << std::endl;

  RUN_TEST(test_to_lower);
  RUN_TEST(test_matches_pattern_case_sensitive);
  RUN_TEST(test_matches_pattern_case_insensitive);
  RUN_TEST(test_matches_pattern_whole_word);
  RUN_TEST(test_matches_pattern_regex);
  RUN_TEST(test_matches_pattern_regex_case_insensitive);
  RUN_TEST(test_matches_pattern_multiple_matches);
  RUN_TEST(test_matches_pattern_utf8);
  RUN_TEST(test_matches_pattern_invalid_regex);
  RUN_TEST(test_search_single_file);
  RUN_TEST(test_search_multiple_files);
  RUN_TEST(test_search_case_insensitive);
  RUN_TEST(test_search_whole_word);
  RUN_TEST(test_search_regex);
  RUN_TEST(test_search_max_results);
  RUN_TEST(test_search_no_matches);
  RUN_TEST(test_search_utf8_content);

  std::cout << "✓ All simple_search_backend tests passed" << std::endl;
  return 0;
}
