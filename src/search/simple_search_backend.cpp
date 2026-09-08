#include "simple_search_backend.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <iconv.h>
#endif

#include "search_file_info.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/string_utils.h"
#include "utils/utils.h"

namespace vxcore {

namespace {

std::atomic<bool> g_probe_armed{false};
std::atomic<bool> g_scan_throw_armed{false};
std::mutex g_probe_mu;
std::condition_variable g_probe_cv;
std::set<std::thread::id> g_probe_thread_ids;
size_t g_probe_expected = 0;
size_t g_probe_arrived = 0;
constexpr size_t kMaxSearchFileSize = 50U * 1024U * 1024U;

const std::vector<std::string> &DefaultSearchEncodings() {
  static const std::vector<std::string> encodings{"UTF-8", "GB18030"};
  return encodings;
}

std::string NormalizeEncodingName(const std::string &name) {
  std::string normalized;
  normalized.reserve(name.size());
  for (unsigned char ch : name) {
    if (ch != '-' && ch != '_' && !std::isspace(ch)) {
      normalized.push_back(static_cast<char>(std::toupper(ch)));
    }
  }
  return normalized;
}

bool DecodeUtf8Scalar(const std::string &text, size_t *offset, uint32_t *scalar) {
  const size_t i = *offset;
  if (i >= text.size()) {
    return false;
  }

  const auto byte = [&text](size_t index) { return static_cast<unsigned char>(text[index]); };
  const unsigned char first = byte(i);
  if (first <= 0x7F) {
    *scalar = first;
    *offset = i + 1;
    return true;
  }

  size_t length = 0;
  uint32_t value = 0;
  if (first >= 0xC2 && first <= 0xDF) {
    length = 2;
    value = first & 0x1F;
  } else if (first >= 0xE0 && first <= 0xEF) {
    length = 3;
    value = first & 0x0F;
  } else if (first >= 0xF0 && first <= 0xF4) {
    length = 4;
    value = first & 0x07;
  } else {
    return false;
  }

  if (i + length > text.size()) {
    return false;
  }
  for (size_t j = 1; j < length; ++j) {
    const unsigned char continuation = byte(i + j);
    if ((continuation & 0xC0) != 0x80) {
      return false;
    }
    value = (value << 6) | (continuation & 0x3F);
  }

  if ((length == 3 && value < 0x800) || (length == 4 && value < 0x10000) ||
      (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF) {
    return false;
  }

  *scalar = value;
  *offset = i + length;
  return true;
}

bool IsValidUtf8(const std::string &text) {
  size_t offset = 0;
  while (offset < text.size()) {
    uint32_t scalar = 0;
    if (!DecodeUtf8Scalar(text, &offset, &scalar)) {
      return false;
    }
  }
  return true;
}

int Utf16ColumnForByteOffset(const std::string &text, size_t byte_offset) {
  size_t offset = 0;
  int units = 0;
  const size_t limit = std::min(byte_offset, text.size());
  while (offset < limit) {
    uint32_t scalar = 0;
    const size_t before = offset;
    if (!DecodeUtf8Scalar(text, &offset, &scalar) || offset > limit) {
      offset = before + 1;
      ++units;
      continue;
    }
    units += scalar > 0xFFFF ? 2 : 1;
  }
  return units;
}

bool DecodeGb18030(const std::string &input, std::string &output) {
  if (input.empty()) {
    output.clear();
    return true;
  }

#ifdef _WIN32
  if (input.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  constexpr UINT kGb18030CodePage = 54936;
  const int input_size = static_cast<int>(input.size());
  const int wide_size = MultiByteToWideChar(kGb18030CodePage, MB_ERR_INVALID_CHARS, input.data(),
                                            input_size, nullptr, 0);
  if (wide_size <= 0) {
    return false;
  }

  std::wstring wide(static_cast<size_t>(wide_size), L'\0');
  if (MultiByteToWideChar(kGb18030CodePage, MB_ERR_INVALID_CHARS, input.data(), input_size,
                          wide.data(), wide_size) != wide_size) {
    return false;
  }

  const int utf8_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide_size,
                                            nullptr, 0, nullptr, nullptr);
  if (utf8_size <= 0) {
    return false;
  }
  output.resize(static_cast<size_t>(utf8_size));
  return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide_size, output.data(),
                             utf8_size, nullptr, nullptr) == utf8_size;
#else
  iconv_t converter = iconv_open("UTF-8", "GB18030");
  if (converter == reinterpret_cast<iconv_t>(-1)) {
    return false;
  }

  std::vector<char> converted(input.size() * 4 + 16);
  char *input_ptr = const_cast<char *>(input.data());
  size_t input_left = input.size();
  char *output_ptr = converted.data();
  size_t output_left = converted.size();
  const size_t result = iconv(converter, &input_ptr, &input_left, &output_ptr, &output_left);
  iconv_close(converter);
  if (result != 0 || input_left != 0) {
    return false;
  }

  output.assign(converted.data(), converted.size() - output_left);
  return true;
#endif
}

bool DecodeSearchText(const std::string &input, const std::vector<std::string> &encodings,
                      std::string &output) {
  for (const auto &encoding : encodings) {
    const std::string normalized = NormalizeEncodingName(encoding);
    if (normalized == "UTF8") {
      const size_t bom_size = input.size() >= 3 && static_cast<unsigned char>(input[0]) == 0xEF &&
                                      static_cast<unsigned char>(input[1]) == 0xBB &&
                                      static_cast<unsigned char>(input[2]) == 0xBF
                                  ? 3
                                  : 0;
      const std::string text = input.substr(bom_size);
      if (IsValidUtf8(text)) {
        output = text;
        return true;
      }
    } else if (normalized == "GB18030" && DecodeGb18030(input, output)) {
      return true;
    }
  }
  return false;
}

bool ReadSearchText(const std::string &path, const std::vector<std::string> &encodings,
                    std::string &output) {
  std::ifstream file(PathFromUtf8(path), std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return false;
  }

  const std::streampos end = file.tellg();
  if (end < 0 || static_cast<uint64_t>(end) > kMaxSearchFileSize) {
    return false;
  }
  std::string bytes(static_cast<size_t>(end), '\0');
  file.seekg(0, std::ios::beg);
  if (!bytes.empty() && !file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
    return false;
  }
  if (bytes.size() >= 8 &&
      (std::memcmp(bytes.data(), "VNOTEE1\0", 8) == 0 ||
       std::memcmp(bytes.data(), "VNEKEY1\0", 8) == 0)) {
    output.clear();
    return false;
  }
  return DecodeSearchText(bytes, encodings, output);
}

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
SimpleSearchBackend::SimpleSearchBackend(std::vector<std::string> encodings)
    : encodings_(encodings.empty() ? DefaultSearchEncodings() : std::move(encodings)) {}

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
    match.column_start = Utf16ColumnForByteOffset(line, static_cast<size_t>(it->position()));
    match.column_end = Utf16ColumnForByteOffset(
        line, static_cast<size_t>(it->position()) + static_cast<size_t>(it->length()));
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
    match.column_start = Utf16ColumnForByteOffset(line, pos);
    match.column_end = Utf16ColumnForByteOffset(line, pos + transformed_pattern.length());
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

bool SimpleSearchBackend::RunMatch(const MatchContext &ctx, const std::string &line,
                                   int line_number, std::vector<SearchMatch> &out_matches) {
  if (ctx.regex) {
    return DoRegexMatch(ctx.pattern_regex, line, line_number, out_matches);
  }
  if (!ctx.case_sensitive) {
    return DoPatternMatch(ctx.lowercased_pattern, false, ctx.whole_word, line, line_number,
                          out_matches);
  }
  return DoPatternMatch(ctx.literal_pattern, true, ctx.whole_word, line, line_number, out_matches);
}

VxCoreError SimpleSearchBackend::BuildMatchContext(
    const std::string &pattern, SearchOption options,
    const std::vector<std::string> &content_exclude_patterns, MatchContext &out_ctx) {
  out_ctx.case_sensitive = HasFlag(options, SearchOption::kCaseSensitive);
  out_ctx.whole_word = HasFlag(options, SearchOption::kWholeWord);
  out_ctx.regex = HasFlag(options, SearchOption::kRegex);

  if (out_ctx.regex) {
    try {
      out_ctx.pattern_regex = std::regex(
          pattern, out_ctx.case_sensitive ? std::regex::ECMAScript
                                          : (std::regex::ECMAScript | std::regex::icase));
    } catch (const std::regex_error &) {
      return VXCORE_ERR_INVALID_PARAM;
    }
  } else if (!out_ctx.case_sensitive) {
    out_ctx.lowercased_pattern = ToLowerString(pattern);
  } else {
    out_ctx.literal_pattern = pattern;
  }

  out_ctx.content_exclude_patterns = content_exclude_patterns;
  return PreprocessExcludePatterns(content_exclude_patterns, out_ctx.case_sensitive, out_ctx.regex,
                                   out_ctx.lowercased_exclude_patterns, out_ctx.exclude_regexes);
}

void SimpleSearchBackend::ScanChunk(const std::vector<SearchFileInfo> &files, size_t begin,
                                    size_t end, const MatchContext &ctx,
                                    std::vector<ContentSearchMatchedFile> &out_files) {
  // Fired once per chunk (parity with the former per-work-item probe) so the T6
  // concurrency/exception test seams still exercise the drain path.
  RunWorkItemProbeHook();

  for (size_t i = begin; i < end; ++i) {
    if (cancel_flag_ && *cancel_flag_ != 0) {
      return;
    }

    const auto &file_info = files[i];
    std::string text;
    if (!ReadSearchText(file_info.absolute_path, encodings_, text)) {
      VXCORE_LOG_WARN("Skipping undecodable search file: %s", file_info.absolute_path.c_str());
      continue;
    }

    std::vector<SearchMatch> file_matches;
    size_t line_start = 0;
    int line_number = 0;
    while (line_start <= text.size()) {
      const size_t newline = text.find('\n', line_start);
      const size_t line_end = newline == std::string::npos ? text.size() : newline;
      std::string line = text.substr(line_start, line_end - line_start);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      ++line_number;

      if (!IsLineExcluded(line, ctx.content_exclude_patterns, ctx.lowercased_exclude_patterns,
                          ctx.exclude_regexes)) {
        std::vector<SearchMatch> line_matches;
        if (RunMatch(ctx, line, line_number, line_matches)) {
          for (auto &match : line_matches) {
            file_matches.push_back(std::move(match));
          }
        }
      }

      if (newline == std::string::npos) {
        break;
      }
      line_start = newline + 1;
    }

    if (!file_matches.empty()) {
      ContentSearchMatchedFile matched_file;
      matched_file.path = file_info.path;
      matched_file.id = file_info.id;
      matched_file.matches = std::move(file_matches);
      out_files.push_back(std::move(matched_file));
    }
  }
}

VxCoreError SimpleSearchBackend::SearchStreaming(
    const std::vector<SearchFileInfo> &files, const std::string &pattern, SearchOption options,
    const std::vector<std::string> &content_exclude_patterns, int batch_size,
    const SearchBatchEmitFn &emit_batch) {
  const size_t effective_batch = batch_size > 0 ? static_cast<size_t>(batch_size)
                                                : static_cast<size_t>(kDefaultSearchChunkSize);
  const size_t file_count = files.size();
  if (file_count == 0) {
    // Zero files -> total_batches == 0, no callbacks (contract).
    return VXCORE_OK;
  }

  const int total_batches = static_cast<int>((file_count + effective_batch - 1) / effective_batch);

  // An empty pattern can never match. Still fire one empty batch per chunk so a streaming
  // consumer observes the full sweep (uniform progress) without any filesystem I/O.
  if (pattern.empty()) {
    for (int b = 0; b < total_batches; ++b) {
      if (cancel_flag_ && *cancel_flag_ != 0) {
        return VXCORE_ERR_CANCELLED;
      }
      std::vector<ContentSearchMatchedFile> empty_batch;
      emit_batch(b, total_batches, empty_batch);
    }
    return VXCORE_OK;
  }

  MatchContext ctx;
  VxCoreError build_err = BuildMatchContext(pattern, options, content_exclude_patterns, ctx);
  if (build_err != VXCORE_OK) {
    return build_err;
  }

  // Single chunk, or no work queue to fan out onto: scan chunks inline, in order, on the
  // calling thread. A scan-path exception propagates directly to the caller.
  if (total_batches == 1 || work_queue_ == nullptr) {
    for (int b = 0; b < total_batches; ++b) {
      if (cancel_flag_ && *cancel_flag_ != 0) {
        return VXCORE_ERR_CANCELLED;
      }
      const size_t begin = static_cast<size_t>(b) * effective_batch;
      const size_t end = std::min(begin + effective_batch, file_count);
      std::vector<ContentSearchMatchedFile> batch_files;
      ScanChunk(files, begin, end, ctx, batch_files);
      emit_batch(b, total_batches, batch_files);
    }
    if (cancel_flag_ && *cancel_flag_ != 0) {
      return VXCORE_ERR_CANCELLED;
    }
    return VXCORE_OK;
  }

  // Multiple chunks with a work queue: enqueue one item per chunk; the initiator help-drains.
  constexpr int kHelpDrainPollMs = 5;
  std::atomic<int> remaining{total_batches};
  std::mutex exc_mu;
  std::exception_ptr first_exc;

  for (int b = 0; b < total_batches; ++b) {
    auto work = [&, b]() {
      // Decrement remaining on EVERY exit path (cancel-skip, exception, normal completion)
      // so the drain loop can never hang.
      struct RemainingGuard {
        std::atomic<int> &remaining;
        ~RemainingGuard() { remaining.fetch_sub(1, std::memory_order_release); }
      } guard{remaining};

      try {
        // Probe hook must fire even on the cancel-skip path so the T6 parallelism barrier
        // (which arms with cancellation disabled) always reaches its expected arrivals.
        if (cancel_flag_ && *cancel_flag_ != 0) {
          RunWorkItemProbeHook();
          return;
        }

        const size_t begin = static_cast<size_t>(b) * effective_batch;
        const size_t end = std::min(begin + effective_batch, file_count);
        std::vector<ContentSearchMatchedFile> batch_files;
        ScanChunk(files, begin, end, ctx, batch_files);
        // Fired from a worker thread; MAY run concurrently with other chunks' callbacks.
        emit_batch(b, total_batches, batch_files);
      } catch (...) {
        std::lock_guard<std::mutex> lk(exc_mu);
        if (!first_exc) {
          first_exc = std::current_exception();
        }
      }
    };

    if (!work_queue_->Enqueue(std::move(work))) {
      // Enqueue failed (queue shut down): the item will never run, so account for it here —
      // never strand the remaining counter.
      remaining.fetch_sub(1, std::memory_order_release);
    }
  }

  // Caller-helps-drain: poll with a small POSITIVE timeout so the initiator does not block on
  // an empty queue while other threads still hold in-flight work.
  while (remaining.load(std::memory_order_acquire) > 0) {
    work_queue_->ProcessNext(kHelpDrainPollMs);
  }

  // Rethrow on the initiator thread so it propagates through Search ->
  // SearchManager::SearchContent catch(const std::exception&) -> ERR_UNKNOWN.
  if (first_exc) {
    std::rethrow_exception(first_exc);
  }

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

  // Accumulate the streamed chunk slices by batch_index. Callbacks may fire concurrently on
  // the enqueued path, so guard the store with a mutex. Reassembly happens after draining.
  std::vector<std::vector<ContentSearchMatchedFile>> chunks;
  std::mutex chunks_mu;

  SearchBatchEmitFn accumulate = [&](int batch_index, int total_batches,
                                     std::vector<ContentSearchMatchedFile> &batch_files) {
    std::lock_guard<std::mutex> lk(chunks_mu);
    if (chunks.empty() && total_batches > 0) {
      chunks.resize(static_cast<size_t>(total_batches));
    }
    if (batch_index >= 0 && batch_index < static_cast<int>(chunks.size())) {
      chunks[static_cast<size_t>(batch_index)] = std::move(batch_files);
    }
  };

  VxCoreError err = SearchStreaming(files, pattern, options, content_exclude_patterns,
                                    /*batch_size=*/0, accumulate);
  if (err != VXCORE_OK) {
    // CANCELLED / INVALID_PARAM: leave out_result cleared (parity with the pre-streaming
    // path, whose partial results were discarded by SearchManager::SearchContent on error).
    return err;
  }

  // Reassemble in input-file order: ascending batch_index, in-chunk order.
  for (auto &chunk : chunks) {
    for (auto &matched_file : chunk) {
      out_result.matched_files.push_back(std::move(matched_file));
    }
  }

  // Deterministic file-boundary max_results truncation (byte-identical to the former path).
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
          return VXCORE_OK;
        }
      }
    }
  }

  return VXCORE_OK;
}

}  // namespace vxcore
