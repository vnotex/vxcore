#include "simple_search_backend.h"

#include <cctype>
#include <fstream>

#include "search_file_info.h"
#include "utils/string_utils.h"
#include "utils/utils.h"

namespace vxcore {

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

  int total_matches = 0;
  for (const auto &file_info : files) {
    if (out_result.truncated) {
      break;
    }

    std::ifstream file(file_info.absolute_path);
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

}  // namespace vxcore
