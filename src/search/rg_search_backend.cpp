#include "rg_search_backend.h"

#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "platform/process_utils.h"
#include "search_file_info.h"
#include "utils/file_utils.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

RgSearchBackend::RgSearchBackend() {}

RgSearchBackend::~RgSearchBackend() = default;

bool RgSearchBackend::IsAvailable() { return ProcessUtils::IsCommandAvailable("rg"); }

VxCoreError RgSearchBackend::Search(const std::vector<SearchFileInfo> &files,
                                    const std::string &pattern, SearchOption options,
                                    const std::vector<std::string> &content_exclude_patterns,
                                    int max_results, ContentSearchResult &out_result) {
  out_result.matched_files.clear();
  out_result.truncated = false;

  if (files.empty()) {
    return VXCORE_OK;
  }

  std::unordered_map<std::string, const SearchFileInfo *> abs_to_file_info;
  for (const auto &file_info : files) {
    // |file_info.absolute_path| is assumed to be already cleaned.
    abs_to_file_info[file_info.absolute_path] = &file_info;
  }

  std::string command =
      BuildCommand(files, pattern, options, content_exclude_patterns, max_results);

  VXCORE_LOG_DEBUG("Executing search command: %s", command.c_str());

  ProcessUtils::ProcessResult proc_result;
  if (!ProcessUtils::ExecuteCommand(command, proc_result)) {
    VXCORE_LOG_ERROR("Failed to execute search command");
    return VXCORE_ERR_IO;
  }

  if (proc_result.exit_code != 0 && proc_result.exit_code != 1) {
    VXCORE_LOG_ERROR("Search command failed with exit code: %d", proc_result.exit_code);
    return VXCORE_ERR_IO;
  }

  std::vector<ContentSearchMatchedFile> raw_results;
  ParseOutput(proc_result.output, abs_to_file_info, raw_results);

  // Apply max_results limit (counts total matches across all files)
  if (max_results > 0) {
    int total_matches = 0;
    for (auto &matched_file : raw_results) {
      if (total_matches >= max_results) {
        out_result.truncated = true;
        break;
      }

      int remaining = max_results - total_matches;
      if (static_cast<int>(matched_file.matches.size()) > remaining) {
        matched_file.matches.resize(remaining);
        out_result.truncated = true;
      }

      total_matches += static_cast<int>(matched_file.matches.size());
      out_result.matched_files.push_back(std::move(matched_file));
    }
  } else {
    out_result.matched_files = std::move(raw_results);
  }

  return VXCORE_OK;
}

std::string RgSearchBackend::BuildCommand(const std::vector<SearchFileInfo> &files,
                                          const std::string &pattern, SearchOption options,
                                          const std::vector<std::string> &content_exclude_patterns,
                                          int max_results) {
  std::ostringstream cmd;
  cmd << "rg --json --no-heading --with-filename --line-number --column";

  if (!HasFlag(options, SearchOption::kCaseSensitive)) {
    cmd << " --ignore-case";
  }

  if (HasFlag(options, SearchOption::kWholeWord)) {
    cmd << " --word-regexp";
  }

  if (!HasFlag(options, SearchOption::kRegex)) {
    cmd << " --fixed-strings";
  }

  unsigned int thread_count = std::thread::hardware_concurrency();
  if (thread_count > 1) {
    cmd << " --threads " << thread_count;
  }

  cmd << " --max-filesize 50M";

  if (max_results > 0) {
    cmd << " --max-count " << (max_results * 2);
  }

  if (!content_exclude_patterns.empty()) {
    cmd << " --invert-match";
    for (const auto &exclude_pattern : content_exclude_patterns) {
      if (HasFlag(options, SearchOption::kRegex)) {
        cmd << " -e ";
      }
      cmd << ProcessUtils::EscapeShellArg(exclude_pattern);
    }
  }

  cmd << " -- " << ProcessUtils::EscapeShellArg(pattern);

  for (const auto &finfo : files) {
    cmd << " " << ProcessUtils::EscapeShellArg(finfo.absolute_path);
  }

  VXCORE_LOG_DEBUG("Constructed rg command: %s", cmd.str().c_str());

  return cmd.str();
}

void RgSearchBackend::ParseOutput(
    const std::string &output,
    const std::unordered_map<std::string, const SearchFileInfo *> &abs_to_file_info,
    std::vector<ContentSearchMatchedFile> &out_results) {
  std::istringstream stream(output);
  std::string line;

  ContentSearchMatchedFile current_result;
  std::string current_file;

  while (std::getline(stream, line)) {
    if (line.empty()) {
      continue;
    }

    try {
      auto json = nlohmann::json::parse(line);

      if (!json.contains("type")) {
        continue;
      }

      std::string type = json["type"].get<std::string>();

      if (type == "match") {
        std::string absolute_file_path = json["data"]["path"]["text"].get<std::string>();

        if (absolute_file_path != current_file) {
          if (!current_result.path.empty()) {
            out_results.push_back(std::move(current_result));
          }
          current_result = ContentSearchMatchedFile();

          std::string normalized_path = CleanPath(absolute_file_path);
          auto it = abs_to_file_info.find(normalized_path);
          if (it != abs_to_file_info.end()) {
            current_result.path = it->second->path;
            current_result.id = it->second->id;
          } else {
            current_result.path = absolute_file_path;
          }
          current_file = absolute_file_path;
        }

        SearchMatch match;
        match.line_number = json["data"]["line_number"].get<int>();

        auto &submatches = json["data"]["submatches"];
        if (submatches.is_array() && !submatches.empty()) {
          auto &submatch = submatches[0];
          match.column_start = submatch["start"].get<int>() + 1;
          match.column_end = submatch["end"].get<int>() + 1;
        }

        if (json["data"]["lines"].contains("text")) {
          match.line_text = json["data"]["lines"]["text"].get<std::string>();
          if (!match.line_text.empty() && match.line_text.back() == '\n') {
            match.line_text.pop_back();
          }
        }

        current_result.matches.push_back(match);
      }
    } catch (const std::exception &e) {
      VXCORE_LOG_WARN("Failed to parse rg output line: %s", e.what());
    }
  }

  if (!current_result.path.empty()) {
    out_results.push_back(current_result);
  }
}

}  // namespace vxcore
