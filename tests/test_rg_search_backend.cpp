#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

#include "search/rg_search_backend.h"
#include "search/search_file_info.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "vxcore/vxcore_types.h"

using namespace vxcore;

class RgSearchBackendTest {
 public:
  static bool IsAvailable(RgSearchBackend &backend) { return backend.IsAvailable(); }

  static std::string BuildCommand(RgSearchBackend &backend,
                                  const std::vector<SearchFileInfo> &files,
                                  const std::string &pattern, SearchOption options,
                                  const std::vector<std::string> &content_exclude_patterns,
                                  int max_results) {
    return backend.BuildCommand(files, pattern, options, content_exclude_patterns, max_results);
  }

  static void ParseOutput(
      RgSearchBackend &backend, const std::string &output,
      const std::unordered_map<std::string, const SearchFileInfo *> &abs_to_file_info,
      std::vector<ContentSearchMatchedFile> &out_results) {
    backend.ParseOutput(output, abs_to_file_info, out_results);
  }
};

int test_is_available() {
  std::cout << "  Running test_is_available..." << std::endl;

  RgSearchBackend backend;
  bool available = RgSearchBackendTest::IsAvailable(backend);

  std::cout << "    rg available: " << (available ? "yes" : "no") << std::endl;

  std::cout << "  ✓ test_is_available passed" << std::endl;
  return 0;
}

int test_build_command_basic() {
  std::cout << "  Running test_build_command_basic..." << std::endl;

  RgSearchBackend backend;
  std::vector<SearchFileInfo> files;
  files.push_back(SearchFileInfo{"/path/to/file.txt", "/absolute/path/to/file.txt"});

  std::string cmd = RgSearchBackendTest::BuildCommand(backend, files, "search_term",
                                                      SearchOption::kCaseSensitive, {}, 100);

  ASSERT_TRUE(cmd.find("rg") != std::string::npos);
  ASSERT_TRUE(cmd.find("--json") != std::string::npos);
  ASSERT_TRUE(cmd.find("--fixed-strings") != std::string::npos);
  ASSERT_TRUE(cmd.find("search_term") != std::string::npos);
  ASSERT_TRUE(cmd.find("file.txt") != std::string::npos);

  std::cout << "  ✓ test_build_command_basic passed" << std::endl;
  return 0;
}

int test_build_command_case_insensitive() {
  std::cout << "  Running test_build_command_case_insensitive..." << std::endl;

  RgSearchBackend backend;
  std::vector<SearchFileInfo> files;
  files.push_back(SearchFileInfo{"/path/to/file.txt", "/absolute/path/to/file.txt"});

  std::string cmd =
      RgSearchBackendTest::BuildCommand(backend, files, "test", SearchOption::kNone, {}, 100);

  ASSERT_TRUE(cmd.find("--ignore-case") != std::string::npos);

  std::cout << "  ✓ test_build_command_case_insensitive passed" << std::endl;
  return 0;
}

int test_build_command_whole_word() {
  std::cout << "  Running test_build_command_whole_word..." << std::endl;

  RgSearchBackend backend;
  std::vector<SearchFileInfo> files;
  files.push_back(SearchFileInfo{"/path/to/file.txt", "/absolute/path/to/file.txt"});

  std::string cmd = RgSearchBackendTest::BuildCommand(
      backend, files, "test", SearchOption::kCaseSensitive | SearchOption::kWholeWord, {}, 100);

  ASSERT_TRUE(cmd.find("--word-regexp") != std::string::npos);

  std::cout << "  ✓ test_build_command_whole_word passed" << std::endl;
  return 0;
}

int test_build_command_regex() {
  std::cout << "  Running test_build_command_regex..." << std::endl;

  RgSearchBackend backend;
  std::vector<SearchFileInfo> files;
  files.push_back(SearchFileInfo{"/path/to/file.txt", "/absolute/path/to/file.txt"});

  std::string cmd = RgSearchBackendTest::BuildCommand(
      backend, files, "test.*", SearchOption::kCaseSensitive | SearchOption::kRegex, {}, 100);

  ASSERT_TRUE(cmd.find("--fixed-strings") == std::string::npos);

  std::cout << "  ✓ test_build_command_regex passed" << std::endl;
  return 0;
}

int test_build_command_with_exclude() {
  std::cout << "  Running test_build_command_with_exclude..." << std::endl;

  RgSearchBackend backend;
  std::vector<SearchFileInfo> files;
  files.push_back(SearchFileInfo{"/path/to/file.txt", "/absolute/path/to/file.txt"});

  std::vector<std::string> exclude_patterns = {"exclude1", "exclude2"};
  std::string cmd = RgSearchBackendTest::BuildCommand(
      backend, files, "test", SearchOption::kCaseSensitive | SearchOption::kRegex, exclude_patterns,
      100);

  ASSERT_TRUE(cmd.find("--invert-match") != std::string::npos);
  ASSERT_TRUE(cmd.find("exclude1") != std::string::npos);
  ASSERT_TRUE(cmd.find("exclude2") != std::string::npos);

  std::cout << "  ✓ test_build_command_with_exclude passed" << std::endl;
  return 0;
}

int test_build_command_utf8_pattern() {
  std::cout << "  Running test_build_command_utf8_pattern..." << std::endl;

  RgSearchBackend backend;
  std::vector<SearchFileInfo> files;
  files.push_back(SearchFileInfo{"/path/to/file.txt", "/absolute/path/to/file.txt"});

  std::string utf8_pattern = "你好世界";
  std::string cmd = RgSearchBackendTest::BuildCommand(backend, files, utf8_pattern,
                                                      SearchOption::kCaseSensitive, {}, 100);

  ASSERT_TRUE(cmd.find(utf8_pattern) != std::string::npos);

  std::cout << "  ✓ test_build_command_utf8_pattern passed" << std::endl;
  return 0;
}

int test_build_command_utf8_filename() {
  std::cout << "  Running test_build_command_utf8_filename..." << std::endl;

  RgSearchBackend backend;
  std::vector<SearchFileInfo> files;
  std::string utf8_filename = "/path/to/文件.txt";
  files.push_back(SearchFileInfo{utf8_filename, utf8_filename});

  std::string cmd = RgSearchBackendTest::BuildCommand(backend, files, "test",
                                                      SearchOption::kCaseSensitive, {}, 100);

  ASSERT_TRUE(cmd.find(utf8_filename) != std::string::npos);

  std::cout << "  ✓ test_build_command_utf8_filename passed" << std::endl;
  return 0;
}

int test_parse_output_empty() {
  std::cout << "  Running test_parse_output_empty..." << std::endl;

  RgSearchBackend backend;
  std::vector<ContentSearchMatchedFile> results;
  std::unordered_map<std::string, const SearchFileInfo *> abs_to_file_info;

  RgSearchBackendTest::ParseOutput(backend, "", abs_to_file_info, results);
  ASSERT_TRUE(results.empty());

  std::cout << "  ✓ test_parse_output_empty passed" << std::endl;
  return 0;
}

int test_parse_output_single_match() {
  std::cout << "  Running test_parse_output_single_match..." << std::endl;

  RgSearchBackend backend;
  std::vector<ContentSearchMatchedFile> results;
  std::unordered_map<std::string, const SearchFileInfo *> abs_to_file_info;

  std::string json_output =
      R"({"type":"match","data":{"path":{"text":"test.txt"},"lines":{"text":"hello world\n"},"line_number":1,"absolute_offset":0,"submatches":[{"match":{"text":"hello"},"start":0,"end":5}]}})";

  RgSearchBackendTest::ParseOutput(backend, json_output, abs_to_file_info, results);

  ASSERT_EQ(results.size(), 1);
  ASSERT_EQ(results[0].path, "test.txt");
  ASSERT_EQ(results[0].matches.size(), 1);
  ASSERT_EQ(results[0].matches[0].line_number, 1);
  ASSERT_EQ(results[0].matches[0].column_start, 1);
  ASSERT_EQ(results[0].matches[0].column_end, 6);
  ASSERT_EQ(results[0].matches[0].line_text, "hello world");

  std::cout << "  ✓ test_parse_output_single_match passed" << std::endl;
  return 0;
}

int test_parse_output_multiple_matches() {
  std::cout << "  Running test_parse_output_multiple_matches..." << std::endl;

  RgSearchBackend backend;
  std::vector<ContentSearchMatchedFile> results;
  std::unordered_map<std::string, const SearchFileInfo *> abs_to_file_info;

  std::string json_output =
      R"({"type":"match","data":{"path":{"text":"test.txt"},"lines":{"text":"hello world\n"},"line_number":1,"absolute_offset":0,"submatches":[{"match":{"text":"hello"},"start":0,"end":5}]}})"
      "\n"
      R"({"type":"match","data":{"path":{"text":"test.txt"},"lines":{"text":"world peace\n"},"line_number":2,"absolute_offset":12,"submatches":[{"match":{"text":"world"},"start":0,"end":5}]}})";

  RgSearchBackendTest::ParseOutput(backend, json_output, abs_to_file_info, results);

  ASSERT_EQ(results.size(), 1);
  ASSERT_EQ(results[0].path, "test.txt");
  ASSERT_EQ(results[0].matches.size(), 2);

  std::cout << "  ✓ test_parse_output_multiple_matches passed" << std::endl;
  return 0;
}

int test_parse_output_multiple_files() {
  std::cout << "  Running test_parse_output_multiple_files..." << std::endl;

  RgSearchBackend backend;
  std::vector<ContentSearchMatchedFile> results;
  std::unordered_map<std::string, const SearchFileInfo *> abs_to_file_info;

  std::string json_output =
      R"({"type":"match","data":{"path":{"text":"test1.txt"},"lines":{"text":"hello\n"},"line_number":1,"absolute_offset":0,"submatches":[{"match":{"text":"hello"},"start":0,"end":5}]}})"
      "\n"
      R"({"type":"match","data":{"path":{"text":"test2.txt"},"lines":{"text":"world\n"},"line_number":1,"absolute_offset":0,"submatches":[{"match":{"text":"world"},"start":0,"end":5}]}})";

  RgSearchBackendTest::ParseOutput(backend, json_output, abs_to_file_info, results);

  ASSERT_EQ(results.size(), 2);
  ASSERT_EQ(results[0].path, "test1.txt");
  ASSERT_EQ(results[1].path, "test2.txt");

  std::cout << "  ✓ test_parse_output_multiple_files passed" << std::endl;
  return 0;
}

int test_parse_output_utf8_content() {
  std::cout << "  Running test_parse_output_utf8_content..." << std::endl;

  RgSearchBackend backend;
  std::vector<ContentSearchMatchedFile> results;
  std::unordered_map<std::string, const SearchFileInfo *> abs_to_file_info;

  std::string json_output =
      R"({"type":"match","data":{"path":{"text":"test.txt"},"lines":{"text":"你好世界\n"},"line_number":1,"absolute_offset":0,"submatches":[{"match":{"text":"你好"},"start":0,"end":6}]}})";

  RgSearchBackendTest::ParseOutput(backend, json_output, abs_to_file_info, results);

  ASSERT_EQ(results.size(), 1);
  ASSERT_EQ(results[0].matches.size(), 1);
  ASSERT_EQ(results[0].matches[0].line_text, "你好世界");

  std::cout << "  ✓ test_parse_output_utf8_content passed" << std::endl;
  return 0;
}

int test_search_single_file() {
  std::cout << "  Running test_search_single_file..." << std::endl;

  RgSearchBackend backend;
  if (!RgSearchBackendTest::IsAvailable(backend)) {
    std::cout << "  ⊘ test_search_single_file skipped (rg not available)" << std::endl;
    return 0;
  }

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_rg_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test1.txt");
  write_file(test_file, "hello world\ntest content\nhello again");

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

  RgSearchBackend backend;
  if (!RgSearchBackendTest::IsAvailable(backend)) {
    std::cout << "  ⊘ test_search_multiple_files skipped (rg not available)" << std::endl;
    return 0;
  }

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_rg_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file1 = CleanPath(test_dir + "/file1.txt");
  std::string test_file2 = CleanPath(test_dir + "/file2.txt");
  write_file(test_file1, "hello world\ntest content");
  write_file(test_file2, "another test\nhello there");

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

  RgSearchBackend backend;
  if (!RgSearchBackendTest::IsAvailable(backend)) {
    std::cout << "  ⊘ test_search_case_insensitive skipped (rg not available)" << std::endl;
    return 0;
  }

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_rg_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test.txt");
  write_file(test_file, "Hello World\nHELLO world\nhello WORLD");

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

  RgSearchBackend backend;
  if (!RgSearchBackendTest::IsAvailable(backend)) {
    std::cout << "  ⊘ test_search_whole_word skipped (rg not available)" << std::endl;
    return 0;
  }

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_rg_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = test_dir + "/test.txt";
  write_file(test_file, "hello world\nhelloworld\ntest hello test");

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

  RgSearchBackend backend;
  if (!RgSearchBackendTest::IsAvailable(backend)) {
    std::cout << "  ⊘ test_search_regex skipped (rg not available)" << std::endl;
    return 0;
  }

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_rg_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test.txt");
  write_file(test_file, "test123\ntest456\nhello\ntest");

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

  RgSearchBackend backend;
  if (!RgSearchBackendTest::IsAvailable(backend)) {
    std::cout << "  ⊘ test_search_max_results skipped (rg not available)" << std::endl;
    return 0;
  }

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_rg_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test.txt");
  write_file(test_file, "test\ntest\ntest\ntest\ntest");

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

  RgSearchBackend backend;
  if (!RgSearchBackendTest::IsAvailable(backend)) {
    std::cout << "  ⊘ test_search_no_matches skipped (rg not available)" << std::endl;
    return 0;
  }

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_rg_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test.txt");
  write_file(test_file, "hello world\ntest content");

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

  RgSearchBackend backend;
  if (!RgSearchBackendTest::IsAvailable(backend)) {
    std::cout << "  ⊘ test_search_utf8_content skipped (rg not available)" << std::endl;
    return 0;
  }

#ifdef _WIN32
  // Skip on Windows due to UTF-8 encoding issues with cmd.exe/popen
  // The pattern gets corrupted when passed through _popen on Windows
  std::cout << "  ⊘ test_search_utf8_content skipped (Windows UTF-8 limitation)" << std::endl;
  return 0;
#endif

  std::string test_dir = std::filesystem::temp_directory_path().string() + "/vxcore_test_rg_search";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test.txt");
  write_file(test_file, "你好世界\nこんにちは\n你好朋友");

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
  std::cout << "Running rg_search_backend tests..." << std::endl;

  RUN_TEST(test_is_available);
  RUN_TEST(test_build_command_basic);
  RUN_TEST(test_build_command_case_insensitive);
  RUN_TEST(test_build_command_whole_word);
  RUN_TEST(test_build_command_regex);
  RUN_TEST(test_build_command_with_exclude);
  RUN_TEST(test_build_command_utf8_pattern);
  RUN_TEST(test_build_command_utf8_filename);
  RUN_TEST(test_parse_output_empty);
  RUN_TEST(test_parse_output_single_match);
  RUN_TEST(test_parse_output_multiple_matches);
  RUN_TEST(test_parse_output_multiple_files);
  RUN_TEST(test_parse_output_utf8_content);
  RUN_TEST(test_search_single_file);
  RUN_TEST(test_search_multiple_files);
  RUN_TEST(test_search_case_insensitive);
  RUN_TEST(test_search_whole_word);
  RUN_TEST(test_search_regex);
  RUN_TEST(test_search_max_results);
  RUN_TEST(test_search_no_matches);
  RUN_TEST(test_search_utf8_content);

  std::cout << "✓ All rg_search_backend tests passed" << std::endl;
  return 0;
}
