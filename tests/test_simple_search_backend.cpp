#include <atomic>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/work_queue.h"
#include "search/search_backend.h"
#include "search/search_file_info.h"
#include "search/search_query.h"
#include "search/simple_search_backend.h"
#include "test_utils.h"
#include "utils/file_utils.h"
#include "utils/string_utils.h"

using namespace vxcore;

namespace {

// Thread-safe accumulator for streaming SearchStreaming callbacks. Callbacks may fire
// concurrently across drain threads on the enqueued path, so every mutation is guarded. Chunks
// are stored by batch_index (NOT arrival order) so reassemble() reconstructs input-file order.
struct StreamCollector {
  std::mutex mu;
  std::map<int, std::vector<ContentSearchMatchedFile>> by_index;
  int total_batches = -1;
  int callback_count = 0;

  SearchBatchEmitFn fn() {
    return [this](int batch_index, int total,
                  std::vector<ContentSearchMatchedFile> &batch_files) {
      std::lock_guard<std::mutex> lk(mu);
      total_batches = total;
      ++callback_count;
      by_index[batch_index] = std::move(batch_files);
    };
  }

  // Flatten stored chunks in ascending batch_index order == input-file order.
  std::vector<ContentSearchMatchedFile> reassemble() {
    std::vector<ContentSearchMatchedFile> out;
    for (auto &kv : by_index) {
      for (auto &f : kv.second) {
        out.push_back(f);
      }
    }
    return out;
  }
};

SearchFileInfo make_file(const std::string &rel, const std::string &abs) {
  SearchFileInfo fi;
  fi.path = rel;
  fi.absolute_path = abs;
  fi.is_folder = false;
  return fi;
}

}  // namespace

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

// ---------------------------------------------------------------------------
// SearchStreaming (streaming primitive) tests
// ---------------------------------------------------------------------------

int test_streaming_zero_files() {
  std::cout << "  Running test_streaming_zero_files..." << std::endl;

  SimpleSearchBackend backend;
  std::vector<SearchFileInfo> files;  // empty
  StreamCollector c;

  auto err = backend.SearchStreaming(files, "hello", SearchOption::kCaseSensitive, {}, 0, c.fn());

  // Contract: zero files -> total_batches == 0, NO callbacks.
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(c.callback_count, 0);

  std::cout << "  ✓ test_streaming_zero_files passed" << std::endl;
  return 0;
}

int test_streaming_single_chunk_inline() {
  std::cout << "  Running test_streaming_single_chunk_inline..." << std::endl;

  std::string test_dir =
      std::filesystem::temp_directory_path().string() + "/vxcore_test_simple_stream";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test1.txt");
  write_file(test_file, "hello world\ntest content\nhello again");

  SimpleSearchBackend backend;
  std::vector<SearchFileInfo> files{make_file("test1.txt", test_file)};
  StreamCollector c;

  // batch_size 0 -> default chunk; 1 file <= default -> single inline chunk, no work queue.
  auto err = backend.SearchStreaming(files, "hello", SearchOption::kCaseSensitive, {}, 0, c.fn());

  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(c.callback_count, 1);
  ASSERT_EQ(c.total_batches, 1);

  auto flat = c.reassemble();
  ASSERT_EQ(flat.size(), 1);
  ASSERT_EQ(flat[0].path, "test1.txt");
  ASSERT_EQ(flat[0].matches.size(), 2);

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_streaming_single_chunk_inline passed" << std::endl;
  return 0;
}

int test_streaming_zero_match_chunk_still_fires() {
  std::cout << "  Running test_streaming_zero_match_chunk_still_fires..." << std::endl;

  std::string test_dir =
      std::filesystem::temp_directory_path().string() + "/vxcore_test_simple_stream";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/test.txt");
  write_file(test_file, "nothing here\nno match at all");

  SimpleSearchBackend backend;
  std::vector<SearchFileInfo> files{make_file("test.txt", test_file)};
  StreamCollector c;

  auto err = backend.SearchStreaming(files, "absent", SearchOption::kCaseSensitive, {}, 0, c.fn());

  // Contract: the callback fires EXACTLY ONCE per chunk, INCLUDING zero-match chunks.
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(c.callback_count, 1);
  ASSERT_EQ(c.total_batches, 1);
  ASSERT_TRUE(c.reassemble().empty());

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_streaming_zero_match_chunk_still_fires passed" << std::endl;
  return 0;
}

int test_streaming_batch_size_default_single_chunk() {
  std::cout << "  Running test_streaming_batch_size_default_single_chunk..." << std::endl;

  std::string test_dir =
      std::filesystem::temp_directory_path().string() + "/vxcore_test_simple_stream";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::vector<SearchFileInfo> files;
  for (int i = 0; i < 3; ++i) {
    std::string name = "f" + std::to_string(i) + ".txt";
    std::string abs = CleanPath(test_dir + "/" + name);
    write_file(abs, "match here");
    files.push_back(make_file(name, abs));
  }

  SimpleSearchBackend backend;
  StreamCollector c;

  // batch_size 0 -> default (64). 3 files <= 64 -> a single chunk (subsumes the former
  // sequential/parallel threshold).
  auto err = backend.SearchStreaming(files, "match", SearchOption::kCaseSensitive, {}, 0, c.fn());

  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(c.total_batches, 1);
  ASSERT_EQ(c.callback_count, 1);
  ASSERT_EQ(c.reassemble().size(), 3);

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_streaming_batch_size_default_single_chunk passed" << std::endl;
  return 0;
}

int test_streaming_multichunk_inline_ordering() {
  std::cout << "  Running test_streaming_multichunk_inline_ordering..." << std::endl;

  std::string test_dir =
      std::filesystem::temp_directory_path().string() + "/vxcore_test_simple_stream";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::vector<SearchFileInfo> files;
  for (int i = 0; i < 5; ++i) {
    std::string name = "file" + std::to_string(i) + ".txt";
    std::string abs = CleanPath(test_dir + "/" + name);
    write_file(abs, "needle line");
    files.push_back(make_file(name, abs));
  }

  SimpleSearchBackend backend;  // no work queue set -> inline, in order
  StreamCollector c;

  // batch_size 1 -> 5 chunks, each 1 file. Inline path (work_queue_ == nullptr) runs in order.
  auto err = backend.SearchStreaming(files, "needle", SearchOption::kCaseSensitive, {}, 1, c.fn());

  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(c.total_batches, 5);
  ASSERT_EQ(c.callback_count, 5);

  auto flat = c.reassemble();
  ASSERT_EQ(flat.size(), 5);
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(flat[static_cast<size_t>(i)].path, "file" + std::to_string(i) + ".txt");
  }

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_streaming_multichunk_inline_ordering passed" << std::endl;
  return 0;
}

int test_streaming_multichunk_workqueue_out_of_order() {
  std::cout << "  Running test_streaming_multichunk_workqueue_out_of_order..." << std::endl;

  std::string test_dir =
      std::filesystem::temp_directory_path().string() + "/vxcore_test_simple_stream";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  const int kFileCount = 12;
  std::vector<SearchFileInfo> files;
  for (int i = 0; i < kFileCount; ++i) {
    std::string name = "wq" + std::to_string(i) + ".txt";
    std::string abs = CleanPath(test_dir + "/" + name);
    write_file(abs, "target token");
    files.push_back(make_file(name, abs));
  }

  SimpleSearchBackend backend;
  WorkQueue queue;
  backend.SetWorkQueue(&queue);
  StreamCollector c;

  // External drainers race the initiator's help-drain, so chunks complete out of order. The
  // collector reassembles by batch_index regardless of arrival order.
  std::atomic<bool> running{true};
  std::vector<std::thread> drainers;
  for (int i = 0; i < 3; ++i) {
    drainers.emplace_back([&]() {
      while (running.load(std::memory_order_acquire)) {
        queue.ProcessNext(5);
      }
    });
  }

  // batch_size 1 -> 12 chunks enqueued onto the work queue.
  auto err = backend.SearchStreaming(files, "target", SearchOption::kCaseSensitive, {}, 1, c.fn());

  running.store(false, std::memory_order_release);
  for (auto &t : drainers) {
    t.join();
  }

  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(c.total_batches, kFileCount);
  ASSERT_EQ(c.callback_count, kFileCount);

  auto flat = c.reassemble();
  ASSERT_EQ(flat.size(), static_cast<size_t>(kFileCount));
  // Reassembly is deterministic input-file order despite concurrent completion.
  for (int i = 0; i < kFileCount; ++i) {
    ASSERT_EQ(flat[static_cast<size_t>(i)].path, "wq" + std::to_string(i) + ".txt");
  }

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_streaming_multichunk_workqueue_out_of_order passed" << std::endl;
  return 0;
}

int test_streaming_no_truncation() {
  std::cout << "  Running test_streaming_no_truncation..." << std::endl;

  std::string test_dir =
      std::filesystem::temp_directory_path().string() + "/vxcore_test_simple_stream";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/many.txt");
  write_file(test_file, "hit\nhit\nhit\nhit\nhit");  // 5 matches

  SimpleSearchBackend backend;
  std::vector<SearchFileInfo> files{make_file("many.txt", test_file)};

  // Streaming owns NO truncation: emits all 5 matches regardless of any cap.
  StreamCollector c;
  auto err = backend.SearchStreaming(files, "hit", SearchOption::kCaseSensitive, {}, 0, c.fn());
  ASSERT_EQ(err, VXCORE_OK);
  auto flat = c.reassemble();
  ASSERT_EQ(flat.size(), 1);
  ASSERT_EQ(flat[0].matches.size(), 5);

  // The blob wrapper (Search) DOES truncate at the file boundary.
  ContentSearchResult truncated;
  auto err2 = backend.Search(files, "hit", SearchOption::kCaseSensitive, {}, 3, truncated);
  ASSERT_EQ(err2, VXCORE_OK);
  ASSERT_EQ(truncated.matched_files.size(), 1);
  ASSERT_EQ(truncated.matched_files[0].matches.size(), 3);
  ASSERT_TRUE(truncated.truncated);

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_streaming_no_truncation passed" << std::endl;
  return 0;
}

int test_streaming_blob_parity() {
  std::cout << "  Running test_streaming_blob_parity..." << std::endl;

  std::string test_dir =
      std::filesystem::temp_directory_path().string() + "/vxcore_test_simple_stream";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::vector<SearchFileInfo> files;
  for (int i = 0; i < 7; ++i) {
    std::string name = "p" + std::to_string(i) + ".txt";
    std::string abs = CleanPath(test_dir + "/" + name);
    // Alternate match counts, and one file with no match, to exercise mixed chunks.
    if (i % 3 == 0) {
      write_file(abs, "no relevant content");
    } else {
      write_file(abs, "keyword\nkeyword line two");
    }
    files.push_back(make_file(name, abs));
  }

  SimpleSearchBackend backend;

  // Blob path: unbounded (max_results 100 > total matches) -> no truncation.
  ContentSearchResult blob;
  auto err_blob = backend.Search(files, "keyword", SearchOption::kCaseSensitive, {}, 100, blob);
  ASSERT_EQ(err_blob, VXCORE_OK);
  ASSERT_FALSE(blob.truncated);

  // Streaming path with a small batch size to force multiple chunks (inline, in order).
  StreamCollector c;
  auto err_stream =
      backend.SearchStreaming(files, "keyword", SearchOption::kCaseSensitive, {}, 2, c.fn());
  ASSERT_EQ(err_stream, VXCORE_OK);
  auto flat = c.reassemble();

  // Reassembled streaming result set == blob matched_files (same order, same per-file matches).
  ASSERT_EQ(flat.size(), blob.matched_files.size());
  for (size_t i = 0; i < flat.size(); ++i) {
    ASSERT_EQ(flat[i].path, blob.matched_files[i].path);
    ASSERT_EQ(flat[i].matches.size(), blob.matched_files[i].matches.size());
  }

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_streaming_blob_parity passed" << std::endl;
  return 0;
}

int test_streaming_cancel_preset() {
  std::cout << "  Running test_streaming_cancel_preset..." << std::endl;

  std::string test_dir =
      std::filesystem::temp_directory_path().string() + "/vxcore_test_simple_stream";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/c.txt");
  write_file(test_file, "hello world");

  SimpleSearchBackend backend;
  std::vector<SearchFileInfo> files{make_file("c.txt", test_file)};

  volatile int cancel_flag = 1;  // preset: cancellation observed before any scan
  backend.SetCancelFlag(&cancel_flag);

  StreamCollector c;
  auto err = backend.SearchStreaming(files, "hello", SearchOption::kCaseSensitive, {}, 0, c.fn());

  ASSERT_EQ(err, VXCORE_ERR_CANCELLED);
  ASSERT_EQ(c.callback_count, 0);

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_streaming_cancel_preset passed" << std::endl;
  return 0;
}

int test_streaming_invalid_regex() {
  std::cout << "  Running test_streaming_invalid_regex..." << std::endl;

  std::string test_dir =
      std::filesystem::temp_directory_path().string() + "/vxcore_test_simple_stream";
  cleanup_test_dir(test_dir);
  create_directory(test_dir);

  std::string test_file = CleanPath(test_dir + "/r.txt");
  write_file(test_file, "anything");

  SimpleSearchBackend backend;
  std::vector<SearchFileInfo> files{make_file("r.txt", test_file)};
  StreamCollector c;

  auto err = backend.SearchStreaming(files, "[invalid",
                                     SearchOption::kCaseSensitive | SearchOption::kRegex, {}, 0,
                                     c.fn());

  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(c.callback_count, 0);

  cleanup_test_dir(test_dir);
  std::cout << "  ✓ test_streaming_invalid_regex passed" << std::endl;
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

  // SearchStreaming (streaming primitive) tests.
  RUN_TEST(test_streaming_zero_files);
  RUN_TEST(test_streaming_single_chunk_inline);
  RUN_TEST(test_streaming_zero_match_chunk_still_fires);
  RUN_TEST(test_streaming_batch_size_default_single_chunk);
  RUN_TEST(test_streaming_multichunk_inline_ordering);
  RUN_TEST(test_streaming_multichunk_workqueue_out_of_order);
  RUN_TEST(test_streaming_no_truncation);
  RUN_TEST(test_streaming_blob_parity);
  RUN_TEST(test_streaming_cancel_preset);
  RUN_TEST(test_streaming_invalid_regex);

  std::cout << "✓ All simple_search_backend tests passed" << std::endl;
  return 0;
}
