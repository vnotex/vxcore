#include "simple_search_backend.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "search_file_info.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/string_utils.h"
#include "utils/utils.h"

namespace vxcore {

namespace {

constexpr int kParallelSearchThreshold = 50;

std::atomic<bool> g_probe_armed{false};
std::atomic<bool> g_scan_throw_armed{false};
std::mutex g_probe_mu;
std::condition_variable g_probe_cv;
std::set<std::thread::id> g_probe_thread_ids;
size_t g_probe_expected = 0;
size_t g_probe_arrived = 0;

void RunWorkItemProbeHook() {
  if (g_scan_throw_armed.load(std::memory_order_relaxed) &&
      g_scan_throw_armed.exchange(false, std::memory_order_relaxed)) {
    throw std::runtime_error("vxcore test-induced scan-path exception");
  }

  if (!g_probe_armed.load(std::memory_order_relaxed)) {
    return;
  }

  std::unique_lock<std::mutex> lk(g_probe_mu);
  g_probe_thread_ids.insert(std::this_thread::get_id());
  ++g_probe_arrived;
  if (g_probe_arrived >= g_probe_expected) {
    g_probe_cv.notify_all();
    return;
  }
  g_probe_cv.wait_for(lk, std::chrono::seconds(2),
                      [] { return g_probe_arrived >= g_probe_expected; });
}

}  // namespace

bool DoRegexMatch(const std::regex &pattern_regex, const std::string &line, int line_number,
                  std::vector<SearchMatch> &out_matches) {
  auto begin = std::sregex_iterator(line.begin(), line.end(), pattern_regex);
  auto end = std::sregex_iterator();

  bool has_match = false;
  for (std::sregex_iterator it = begin; it != end; ++it) {
    has_match = true;
    SearchMatch match;
    // TODO: optimize to avoid copying line multiple times
    match.line_text = line;
    match.line_number = line_number;
    match.column_start = static_cast<int>(it->position());
    match.column_end = match.column_start + static_cast<int>(it->length());
    out_matches.push_back(std::move(match));
  }
  return has_match;
}

bool DoPatternMatch(const std::string &transformed_pattern, bool case_sensitive, bool whole_word,
                    const std::string &line, int line_number,
                    std::vector<SearchMatch> &out_matches) {
  std::string lowercased_line = case_sensitive ? std::string() : ToLowerString(line);
  const std::string &search_line = case_sensitive ? line : lowercased_line;

  size_t pos = 0;
  bool has_match = false;
  while ((pos = search_line.find(transformed_pattern, pos)) != std::string::npos) {
    if (whole_word) {
      bool word_start = (pos == 0 || !std::isalnum(static_cast<unsigned char>(line[pos - 1])));
      size_t end_pos = pos + transformed_pattern.length();
      bool word_end =
          (end_pos >= line.length() || !std::isalnum(static_cast<unsigned char>(line[end_pos])));

      if (!word_start || !word_end) {
        pos++;
        continue;
      }
    }

    has_match = true;
    SearchMatch match;
    // TODO: optimize to avoid copying line multiple times
    match.line_text = line;
    match.line_number = line_number;
    match.column_start = static_cast<int>(pos);
    match.column_end = static_cast<int>(pos + transformed_pattern.length());
    out_matches.push_back(std::move(match));

    pos += transformed_pattern.length();
  }

  return has_match;
}

bool SimpleSearchBackend::MatchesPattern(const std::string &line, const std::string &pattern,
                                         SearchOption options,
                                         std::vector<SearchMatch> &out_matches) {
  bool case_sensitive = HasFlag(options, SearchOption::kCaseSensitive);
  bool whole_word = HasFlag(options, SearchOption::kWholeWord);
  bool regex = HasFlag(options, SearchOption::kRegex);

  std::regex pattern_regex;
  std::string lowercased_pattern;
  if (regex) {
    try {
      pattern_regex =
          std::regex(pattern, case_sensitive ? std::regex::ECMAScript
                                             : (std::regex::ECMAScript | std::regex::icase));
    } catch (const std::regex_error &) {
      return false;
    }
  } else if (!case_sensitive) {
    lowercased_pattern = ToLowerString(pattern);
  }

  if (regex) {
    return DoRegexMatch(pattern_regex, line, 0, out_matches);
  } else if (!case_sensitive) {
    return DoPatternMatch(lowercased_pattern, false, whole_word, line, 0, out_matches);
  } else {
    return DoPatternMatch(pattern, true, whole_word, line, 0, out_matches);
  }
}

void SimpleSearchBackend::SetWorkQueue(WorkQueue *queue) { work_queue_ = queue; }

void SimpleSearchBackend::SetCancelFlag(const volatile int *flag) { cancel_flag_ = flag; }

void SimpleSearchBackend::TestArmParallelismProbe(size_t expectedParticipants) {
  std::lock_guard<std::mutex> lk(g_probe_mu);
  g_probe_expected = expectedParticipants;
  g_probe_arrived = 0;
  g_probe_thread_ids.clear();
  g_probe_armed.store(expectedParticipants != 0, std::memory_order_relaxed);
  g_probe_cv.notify_all();
}

size_t SimpleSearchBackend::TestDistinctWorkerThreadCount() {
  std::lock_guard<std::mutex> lk(g_probe_mu);
  return g_probe_thread_ids.size();
}

void SimpleSearchBackend::TestArmScanThrowOnce() {
  g_scan_throw_armed.store(true, std::memory_order_relaxed);
}

VxCoreError SimpleSearchBackend::SearchSequential(
    const std::vector<SearchFileInfo> &files,
    const std::function<bool(const std::string &, int, std::vector<SearchMatch> &)> &do_match,
    const std::vector<std::string> &content_exclude_patterns,
    const std::vector<std::string> &lowercased_exclude_patterns,
    const std::vector<std::regex> &exclude_regexes, int max_results,
    ContentSearchResult &out_result) {
  int total_matches = 0;
  for (const auto &file_info : files) {
    if (out_result.truncated) {
      break;
    }

    std::ifstream file(PathFromUtf8(file_info.absolute_path));
    if (!file.is_open()) {
      continue;
    }

    std::vector<SearchMatch> file_matches;
    std::string line;
    int line_number = 0;

    while (!out_result.truncated && std::getline(file, line)) {
      line_number++;

      if (IsLineExcluded(line, content_exclude_patterns, lowercased_exclude_patterns,
                         exclude_regexes)) {
        continue;
      }

      std::vector<SearchMatch> line_matches;
      if (do_match(line, line_number, line_matches)) {
        for (auto &match : line_matches) {
          total_matches++;
          file_matches.push_back(std::move(match));
          if (max_results > 0 && total_matches >= max_results) {
            out_result.truncated = true;
            break;
          }
        }
      }
    }

    if (!file_matches.empty()) {
      ContentSearchMatchedFile matched_file;
      matched_file.path = file_info.path;
      matched_file.id = file_info.id;
      matched_file.matches = std::move(file_matches);
      out_result.matched_files.push_back(std::move(matched_file));
    }
  }

  return VXCORE_OK;
}

VxCoreError SimpleSearchBackend::SearchParallel(
    const std::vector<SearchFileInfo> &files,
    const std::function<bool(const std::string &, int, std::vector<SearchMatch> &)> &do_match,
    const std::vector<std::string> &content_exclude_patterns,
    const std::vector<std::string> &lowercased_exclude_patterns,
    const std::vector<std::regex> &exclude_regexes, int max_results,
    ContentSearchResult &out_result) {
  constexpr int kHelpDrainPollMs = 5;

  const size_t file_count = files.size();

  // Distinct output slot per file index — no shared mutex: a worker only ever
  // writes slots[i] for its own i, so writes never race.
  std::vector<ContentSearchMatchedFile> slots(file_count);
  std::atomic<int> remaining{static_cast<int>(file_count)};
  std::mutex exc_mu;
  std::exception_ptr first_exc;

  for (size_t i = 0; i < file_count; ++i) {
    auto work = [&, i]() {
      // Decrement remaining on EVERY exit path (cancel-skip, open failure,
      // normal completion, exception) so the drain loop can never hang.
      struct RemainingGuard {
        std::atomic<int> &remaining;
        ~RemainingGuard() { remaining.fetch_sub(1, std::memory_order_release); }
      } guard{remaining};

      try {
        RunWorkItemProbeHook();

        if (cancel_flag_ && *cancel_flag_ != 0) {
          return;
        }

        const auto &file_info = files[i];
        std::ifstream file(PathFromUtf8(file_info.absolute_path));
        if (!file.is_open()) {
          return;
        }

        std::vector<SearchMatch> file_matches;
        std::string line;
        int line_number = 0;

        while (std::getline(file, line)) {
          line_number++;

          if (IsLineExcluded(line, content_exclude_patterns, lowercased_exclude_patterns,
                             exclude_regexes)) {
            continue;
          }

          std::vector<SearchMatch> line_matches;
          if (do_match(line, line_number, line_matches)) {
            for (auto &match : line_matches) {
              file_matches.push_back(std::move(match));
            }
          }
        }

        if (!file_matches.empty()) {
          ContentSearchMatchedFile matched_file;
          matched_file.path = file_info.path;
          matched_file.id = file_info.id;
          matched_file.matches = std::move(file_matches);
          slots[i] = std::move(matched_file);
        }
      } catch (...) {
        std::lock_guard<std::mutex> lk(exc_mu);
        if (!first_exc) {
          first_exc = std::current_exception();
        }
      }
    };

    if (!work_queue_->Enqueue(std::move(work))) {
      // Enqueue failed (queue shut down): the item will never run, so account
      // for it here — never strand the remaining counter.
      remaining.fetch_sub(1, std::memory_order_release);
    }
  }

  // Caller-helps-drain: the initiator processes items until all work completes.
  // External drainers may help concurrently, so poll with a small POSITIVE
  // timeout — a <= 0 timeout would block the initiator on an empty queue while
  // other threads still hold in-flight work.
  while (remaining.load(std::memory_order_acquire) > 0) {
    work_queue_->ProcessNext(kHelpDrainPollMs);
  }

  // Rethrow on the initiator thread so it propagates through Search ->
  // SearchManager::SearchContent catch(const std::exception&) -> ERR_UNKNOWN.
  // Returning an error code here would be swallowed to VXCORE_OK instead.
  if (first_exc) {
    std::rethrow_exception(first_exc);
  }

  for (auto &slot : slots) {
    if (!slot.matches.empty()) {
      out_result.matched_files.push_back(std::move(slot));
    }
  }

  // Scan-all-then-truncate on a file boundary (parity with the old path).
  if (max_results > 0) {
    int total = 0;
    for (size_t i = 0; i < out_result.matched_files.size(); ++i) {
      auto &matched_file = out_result.matched_files[i];
      for (size_t j = 0; j < matched_file.matches.size(); ++j) {
        total++;
        if (total >= max_results) {
          matched_file.matches.resize(j + 1);
          out_result.matched_files.resize(i + 1);
          out_result.truncated = true;
          goto done_truncation;
        }
      }
    }
  }

done_truncation:
  if (cancel_flag_ && *cancel_flag_ != 0) {
    return VXCORE_ERR_CANCELLED;
  }

  return VXCORE_OK;
}

VxCoreError SimpleSearchBackend::Search(const std::vector<SearchFileInfo> &files,
                                        const std::string &pattern, SearchOption options,
                                        const std::vector<std::string> &content_exclude_patterns,
                                        int max_results, ContentSearchResult &out_result) {
  out_result.matched_files.clear();
  out_result.truncated = false;

  if (pattern.empty()) {
    return VXCORE_OK;
  }

  bool case_sensitive = HasFlag(options, SearchOption::kCaseSensitive);
  bool whole_word = HasFlag(options, SearchOption::kWholeWord);
  bool regex = HasFlag(options, SearchOption::kRegex);

  std::regex pattern_regex;
  std::string lowercased_pattern;
  if (regex) {
    try {
      pattern_regex =
          std::regex(pattern, case_sensitive ? std::regex::ECMAScript
                                             : (std::regex::ECMAScript | std::regex::icase));
    } catch (const std::regex_error &) {
      return VXCORE_ERR_INVALID_PARAM;
    }
  } else if (!case_sensitive) {
    lowercased_pattern = ToLowerString(pattern);
  }

  std::function<bool(const std::string &, int, std::vector<SearchMatch> &)> do_match;
  if (regex) {
    do_match = [&pattern_regex](const std::string &line, int line_number,
                                std::vector<SearchMatch> &out_matches) -> bool {
      return DoRegexMatch(pattern_regex, line, line_number, out_matches);
    };
  } else if (!case_sensitive) {
    do_match = [&lowercased_pattern, whole_word](const std::string &line, int line_number,
                                                 std::vector<SearchMatch> &out_matches) -> bool {
      return DoPatternMatch(lowercased_pattern, false, whole_word, line, line_number, out_matches);
    };
  } else {
    do_match = [&pattern, whole_word](const std::string &line, int line_number,
                                      std::vector<SearchMatch> &out_matches) -> bool {
      return DoPatternMatch(pattern, true, whole_word, line, line_number, out_matches);
    };
  }

  std::vector<std::regex> exclude_regexes;
  std::vector<std::string> lowercased_exclude_patterns;
  auto err = PreprocessExcludePatterns(content_exclude_patterns, case_sensitive, regex,
                                       lowercased_exclude_patterns, exclude_regexes);
  if (err != VXCORE_OK) {
    return err;
  }

  if (!work_queue_ || static_cast<int>(files.size()) < kParallelSearchThreshold) {
    VXCORE_LOG_DEBUG("Content search: using sequential path (%zu files)", files.size());
    return SearchSequential(files, do_match, content_exclude_patterns, lowercased_exclude_patterns,
                            exclude_regexes, max_results, out_result);
  }

  VXCORE_LOG_DEBUG("Content search: using parallel path (%zu files)", files.size());
  return SearchParallel(files, do_match, content_exclude_patterns, lowercased_exclude_patterns,
                        exclude_regexes, max_results, out_result);
}

}  // namespace vxcore
