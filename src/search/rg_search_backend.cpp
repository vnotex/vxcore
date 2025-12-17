#include "rg_search_backend.h"

#include <cstdio>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>

#include "utils/logger.h"

namespace vxcore {

RgSearchBackend::RgSearchBackend() {}

RgSearchBackend::~RgSearchBackend() = default;

bool RgSearchBackend::IsAvailable() {
#ifdef _WIN32
  FILE *pipe = _popen("where rg", "r");
#else
  FILE *pipe = popen("which rg", "r");
#endif
  if (!pipe) {
    return false;
  }

  char buffer[128];
  bool found = fgets(buffer, sizeof(buffer), pipe) != nullptr;

#ifdef _WIN32
  _pclose(pipe);
#else
  pclose(pipe);
#endif

  return found;
}

bool RgSearchBackend::Search(const std::string &root_path, const std::string &pattern,
                             bool case_sensitive, bool whole_word, bool regex,
                             const std::vector<std::string> &file_patterns,
                             const std::vector<std::string> &exclude_patterns,
                             std::vector<ContentSearchResult> &out_results) {
  if (!IsAvailable()) {
    VXCORE_LOG_WARN("ripgrep (rg) is not available");
    return false;
  }

  std::string command = BuildCommand(root_path, pattern, case_sensitive, whole_word, regex,
                                     file_patterns, exclude_patterns);

  VXCORE_LOG_DEBUG("Executing search command: %s", command.c_str());

#ifdef _WIN32
  FILE *pipe = _popen(command.c_str(), "r");
#else
  FILE *pipe = popen(command.c_str(), "r");
#endif

  if (!pipe) {
    VXCORE_LOG_ERROR("Failed to execute search command");
    return false;
  }

  std::string output;
  char buffer[4096];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }

#ifdef _WIN32
  int exit_code = _pclose(pipe);
#else
  int exit_code = pclose(pipe);
#endif

  if (exit_code != 0 && exit_code != 1) {
    VXCORE_LOG_ERROR("Search command failed with exit code: %d", exit_code);
    return false;
  }

  ParseOutput(output, out_results);
  return true;
}

std::string RgSearchBackend::BuildCommand(const std::string &root_path, const std::string &pattern,
                                          bool case_sensitive, bool whole_word, bool regex,
                                          const std::vector<std::string> &file_patterns,
                                          const std::vector<std::string> &exclude_patterns) {
  std::ostringstream cmd;
  cmd << "rg --json --no-heading --with-filename --line-number --column";

  if (!case_sensitive) {
    cmd << " --ignore-case";
  }

  if (whole_word) {
    cmd << " --word-regexp";
  }

  if (!regex) {
    cmd << " --fixed-strings";
  }

  for (const auto &exclude : exclude_patterns) {
    cmd << " --glob \"!" << exclude << "\"";
  }

  for (const auto &file_pattern : file_patterns) {
    cmd << " --glob \"" << file_pattern << "\"";
  }

  cmd << " -- \"" << pattern << "\" \"" << root_path << "\"";

  return cmd.str();
}

void RgSearchBackend::ParseOutput(const std::string &output,
                                  std::vector<ContentSearchResult> &out_results) {
  std::istringstream stream(output);
  std::string line;

  ContentSearchResult current_result;
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
        std::string file_path = json["data"]["path"]["text"].get<std::string>();

        if (file_path != current_file) {
          if (!current_result.file_path.empty()) {
            out_results.push_back(current_result);
          }
          current_result = ContentSearchResult();
          current_result.file_path = file_path;
          current_file = file_path;
        }

        SearchMatch match;
        match.line_number = json["data"]["line_number"].get<int>();

        auto &submatches = json["data"]["submatches"];
        if (submatches.is_array() && !submatches.empty()) {
          auto &submatch = submatches[0];
          match.column_start = submatch["start"].get<int>() + 1;
          match.column_end = submatch["end"].get<int>() + 1;
          match.match_text = submatch["match"]["text"].get<std::string>();
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

  if (!current_result.file_path.empty()) {
    out_results.push_back(current_result);
  }
}

}  // namespace vxcore
