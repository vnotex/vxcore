// vxcore Content-Search Validation Tool
//
// Standalone diagnostic that opens a real bundled notebook, runs a content search the way
// VNote does (streaming primitive + multi-threaded drain pool), and cross-checks the results
// against ripgrep (rg) as ground truth. See tools/AGENTS.md for the full contract, the per-mode
// validation fidelity policy, and how to read the three result buckets.
//
// This tool is a runtime diagnostic, NOT a ctest: it needs a real notebook on disk and is not
// registered with add_test. It links vxcore and calls only the public C ABI.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <windows.h>
#include <shellapi.h>  // must follow windows.h
// clang-format on
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "vxcore/vxcore.h"
#include "vxcore/vxcore_log.h"

namespace {

using nlohmann::json;

// ============================================================================
// Platform / string helpers (this code lives OUTSIDE the vxcore DLL, so all
// filesystem access uses the MultiByteToWideChar pattern for UTF-8 safety).
// ============================================================================

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string &s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string WideToUtf8(const std::wstring &w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr,
                              nullptr);
  if (n <= 0) return std::string();
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                      nullptr);
  return s;
}

std::filesystem::path Utf8ToPath(const std::string &utf8) {
  return std::filesystem::path(Utf8ToWide(utf8));
}

std::string PathToUtf8(const std::filesystem::path &p) { return WideToUtf8(p.wstring()); }

bool Utf8PathExists(const std::string &utf8_path) {
  if (utf8_path.empty()) return false;
  std::error_code ec;
  return std::filesystem::exists(Utf8ToPath(utf8_path), ec);
}
#else
std::filesystem::path Utf8ToPath(const std::string &utf8) { return std::filesystem::path(utf8); }

std::string PathToUtf8(const std::filesystem::path &p) { return p.string(); }

bool Utf8PathExists(const std::string &utf8_path) {
  if (utf8_path.empty()) return false;
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(utf8_path), ec);
}
#endif

bool IsWindows() {
#ifdef _WIN32
  return true;
#else
  return false;
#endif
}

std::string ToLowerAscii(std::string s) {
  for (char &c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

// Canonical display form for a notebook-relative path: forward slashes, duplicate/leading/
// trailing separators removed, and "."/".." components resolved. This mirrors the
// lexically_normal() normalization vxcore applies to scope.folderPath and to every stored
// relpath (Notebook::GetCleanRelativePath → CleanPath), so the indexed-set, search-result, and
// rg universes share identical keys. Root ("", ".", "./", "a/..") collapses to "".
std::string CanonRel(const std::string &in) {
  std::string s = in;
  for (char &c : s) {
    if (c == '\\') c = '/';
  }
  std::vector<std::string> parts;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find('/', i);
    if (j == std::string::npos) j = s.size();
    const std::string comp = s.substr(i, j - i);
    if (comp == "..") {
      if (!parts.empty() && parts.back() != "..") parts.pop_back();
    } else if (!comp.empty() && comp != ".") {
      parts.push_back(comp);
    }
    i = j + 1;
  }
  std::string out;
  for (size_t k = 0; k < parts.size(); ++k) {
    if (k) out += '/';
    out += parts[k];
  }
  return out;
}

// Comparison key: canonical form, case-folded on Windows (paths are case-insensitive there).
std::string KeyOf(const std::string &in) {
  std::string s = CanonRel(in);
  if (IsWindows()) s = ToLowerAscii(s);
  return s;
}

bool HasNonAscii(const std::string &s) {
  for (unsigned char c : s) {
    if (c >= 0x80) return true;
  }
  return false;
}

std::string TrimForDisplay(const std::string &in) {
  std::string s = in;
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  size_t start = s.find_first_not_of(" \t");
  if (start == std::string::npos) return std::string();
  s = s.substr(start);
  const size_t kMax = 120;
  if (s.size() > kMax) s = s.substr(0, kMax) + "...";
  return s;
}

// A notebook-relative path is "infrastructure" if it lives under vx_notebook/ or .git/.
bool IsInfrastructure(const std::string &key) {
  return key == "vx_notebook" || key.rfind("vx_notebook/", 0) == 0 || key == ".git" ||
         key.rfind(".git/", 0) == 0;
}

// ============================================================================
// External process capture (for rg). Returns raw UTF-8 stdout + exit code.
// ============================================================================

struct ProcResult {
  bool launched = false;
  int exit_code = -1;
  std::string out;
};

#ifdef _WIN32
// Quote a single argument per the MS C runtime argv parsing rules.
std::wstring QuoteWinArg(const std::wstring &arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return arg;
  }
  std::wstring q = L"\"";
  for (auto it = arg.begin();; ++it) {
    unsigned backslashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++it;
      ++backslashes;
    }
    if (it == arg.end()) {
      q.append(backslashes * 2, L'\\');
      break;
    } else if (*it == L'"') {
      q.append(backslashes * 2 + 1, L'\\');
      q.push_back(*it);
    } else {
      q.append(backslashes, L'\\');
      q.push_back(*it);
    }
  }
  q.push_back(L'"');
  return q;
}

ProcResult RunCapture(const std::vector<std::string> &args) {
  ProcResult res;
  if (args.empty()) return res;

  std::wstring cmd;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) cmd.push_back(L' ');
    cmd += QuoteWinArg(Utf8ToWide(args[i]));
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return res;
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, OPEN_EXISTING, 0, nullptr);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul;
  si.hStdOutput = write_pipe;
  si.hStdError = nul;

  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
  cmd_buf.push_back(L'\0');

  BOOL ok = CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi);
  CloseHandle(write_pipe);
  if (!ok) {
    CloseHandle(read_pipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    return res;  // launched == false → executable not found / failed to start
  }
  res.launched = true;

  char buf[4096];
  DWORD nread = 0;
  while (ReadFile(read_pipe, buf, sizeof(buf), &nread, nullptr) && nread > 0) {
    res.out.append(buf, nread);
  }
  CloseHandle(read_pipe);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  res.exit_code = static_cast<int>(code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  return res;
}
#else
ProcResult RunCapture(const std::vector<std::string> &args) {
  ProcResult res;
  if (args.empty()) return res;

  int fds[2];
  if (pipe(fds) != 0) return res;

  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return res;
  }
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    int nul = open("/dev/null", O_WRONLY);
    if (nul >= 0) dup2(nul, STDERR_FILENO);
    close(fds[0]);
    close(fds[1]);
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);  // exec failed (e.g., rg not found)
  }

  close(fds[1]);
  res.launched = true;
  char buf[4096];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof(buf))) > 0) res.out.append(buf, static_cast<size_t>(n));
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status)) res.exit_code = WEXITSTATUS(status);
  return res;
}
#endif

// ============================================================================
// Options + arg parsing
// ============================================================================

struct Options {
  std::string notebook_root;
  std::string pattern;
  bool has_pattern = false;
  bool case_sensitive = false;
  bool regex = false;
  bool word = false;
  std::string folder = ".";  // scope subtree; "." == whole notebook
  std::string backend = "simple";
  int max_results = 0;  // 0 == uncapped (validation completeness)
  int limit = 50;       // per-bucket detail cap
  bool strict = false;
  bool json_out = false;
  bool verbose = false;  // when false, vxcore console logging is suppressed
  bool help = false;
};

void PrintUsage() {
  std::cout <<
      R"(vxsearch-validate — cross-check vxcore content search against ripgrep (rg)

Usage:
  vxsearch-validate <notebook-root> --pattern <text> [options]

Required:
  <notebook-root>        Path to a bundled notebook (must contain vx_notebook/config.json).
  --pattern <text>       The search pattern.

Query options:
  --case-sensitive       Case-sensitive match (default: insensitive).
  --regex                Treat pattern as a regular expression (diagnostic-only mode).
  --word                 Whole-word match (diagnostic-only mode). Cannot combine with --regex.
  --folder <subtree>     Restrict to a notebook subtree (default: whole notebook).
  --backend simple|rg    vxcore search backend to validate (default: simple).

Comparison / output:
  --max-results <N>      Cap results (file-boundary), mirroring VNote. 0 = uncapped (default).
  --limit <N>            Max detail lines printed per bucket (default: 50).
  --strict               Non-zero exit on ANY non-empty bucket (incl. discovery/diagnostic).
  --json                 Emit a machine-readable JSON report to stdout.
  --verbose              Do not suppress vxcore's internal console logging.
  -h, --help             Show this help.

Exit code: 0 = clean (no pass/fail discrepancies); 1 = pass/fail discrepancies found
(or, with --strict, any discovery gap or diagnostic difference).
)";
}

bool ParseArgs(int argc, char **argv, Options *opts, std::string *err) {
  auto need_value = [&](int &i, const std::string &flag, std::string *out) -> bool {
    if (i + 1 >= argc) {
      *err = flag + " requires a value";
      return false;
    }
    *out = argv[++i];
    return true;
  };

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      opts->help = true;
      return true;
    } else if (a == "--pattern") {
      std::string v;
      if (!need_value(i, a, &v)) return false;
      opts->pattern = v;
      opts->has_pattern = true;
    } else if (a == "--case-sensitive") {
      opts->case_sensitive = true;
    } else if (a == "--regex") {
      opts->regex = true;
    } else if (a == "--word") {
      opts->word = true;
    } else if (a == "--folder") {
      if (!need_value(i, a, &opts->folder)) return false;
    } else if (a == "--backend") {
      if (!need_value(i, a, &opts->backend)) return false;
    } else if (a == "--max-results") {
      std::string v;
      if (!need_value(i, a, &v)) return false;
      try {
        opts->max_results = std::stoi(v);
      } catch (...) {
        *err = "--max-results must be an integer";
        return false;
      }
    } else if (a == "--limit") {
      std::string v;
      if (!need_value(i, a, &v)) return false;
      try {
        opts->limit = std::stoi(v);
      } catch (...) {
        *err = "--limit must be an integer";
        return false;
      }
    } else if (a == "--strict") {
      opts->strict = true;
    } else if (a == "--json") {
      opts->json_out = true;
    } else if (a == "--verbose") {
      opts->verbose = true;
    } else if (!a.empty() && a[0] == '-') {
      *err = "unknown option: " + a;
      return false;
    } else {
      if (!opts->notebook_root.empty()) {
        *err = "unexpected extra positional argument: " + a;
        return false;
      }
      opts->notebook_root = a;
    }
  }

  if (opts->notebook_root.empty()) {
    *err = "missing <notebook-root>";
    return false;
  }
  if (!opts->has_pattern || opts->pattern.empty()) {
    *err = "missing required --pattern";
    return false;
  }
  if (opts->word && opts->regex) {
    *err = "--word cannot be combined with --regex (simple backend ignores wholeWord under regex)";
    return false;
  }
  if (opts->backend != "simple" && opts->backend != "rg") {
    *err = "--backend must be 'simple' or 'rg'";
    return false;
  }
  // A --folder scope must be a notebook subtree; reject above-root traversal rather than
  // silently normalizing it to a different in-notebook folder.
  {
    std::string f = opts->folder;
    for (char &c : f) {
      if (c == '\\') c = '/';
    }
    size_t i = 0;
    while (i < f.size()) {
      size_t j = f.find('/', i);
      if (j == std::string::npos) j = f.size();
      if (f.substr(i, j - i) == "..") {
        *err = "--folder must be a notebook subtree (no '..' components)";
        return false;
      }
      i = j + 1;
    }
  }
  if (opts->limit < 0) opts->limit = 0;
  return true;
}

// Strip a trailing slash/backslash so path joins are clean.
std::string StripTrailingSep(std::string s) {
  while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
  return s;
}

// ============================================================================
// Indexed-set enumeration (recursive folder_list_children).
// ============================================================================

// Join a notebook-relative folder path with a child name, matching vxcore's ConcatenatePaths
// semantics for the root case ("" / "." → bare child name).
std::string JoinRel(const std::string &folder, const std::string &name) {
  if (folder.empty() || folder == ".") return name;
  return folder + "/" + name;
}

struct IndexedSet {
  std::set<std::string> file_keys;           // KeyOf(relpath) for every indexed file
  std::vector<std::string> phantom_folders;  // metadata-present, disk-missing folders
  int file_count = 0;                        // distinct indexed files
};

void EnumerateFolder(VxCoreContextHandle ctx, const std::string &notebook_id,
                     const std::string &folder_path, IndexedSet *out) {
  char *children_json = nullptr;
  const char *req_path = (folder_path.empty() || folder_path == ".") ? "." : folder_path.c_str();
  VxCoreError err = vxcore_folder_list_children(ctx, notebook_id.c_str(), req_path, &children_json);

  if (err == VXCORE_ERR_NODE_NOT_EXISTS) {
    // Phantom folder: metadata present but content dir missing on disk. It contributes no
    // on-disk hits for rg, so record it and move on rather than aborting the walk.
    out->phantom_folders.push_back(CanonRel(folder_path));
    if (children_json) vxcore_string_free(children_json);
    return;
  }
  if (err != VXCORE_OK || !children_json) {
    if (children_json) vxcore_string_free(children_json);
    std::cerr << "warning: failed to list folder '" << CanonRel(folder_path) << "' ("
              << vxcore_error_message(err) << ")\n";
    return;
  }

  std::string body(children_json);
  vxcore_string_free(children_json);

  json parsed;
  try {
    parsed = json::parse(body);
  } catch (const std::exception &e) {
    std::cerr << "warning: bad listing JSON for '" << CanonRel(folder_path) << "': " << e.what()
              << "\n";
    return;
  }

  if (parsed.contains("files") && parsed["files"].is_array()) {
    for (const auto &f : parsed["files"]) {
      if (!f.contains("name") || !f["name"].is_string()) continue;
      const std::string rel = JoinRel(folder_path, f["name"].get<std::string>());
      if (out->file_keys.insert(KeyOf(rel)).second) ++out->file_count;
    }
  }

  std::vector<std::string> subfolders;
  if (parsed.contains("folders") && parsed["folders"].is_array()) {
    for (const auto &d : parsed["folders"]) {
      if (!d.contains("name") || !d["name"].is_string()) continue;
      subfolders.push_back(JoinRel(folder_path, d["name"].get<std::string>()));
    }
  }
  for (const auto &sub : subfolders) {
    EnumerateFolder(ctx, notebook_id, sub, out);
  }
}

// ============================================================================
// VNote-faithful streaming search runner.
// ============================================================================

struct StreamAccum {
  std::mutex mu;
  std::map<int, json> by_index;  // batch_index → parsed batch JSON
  int total_batches = -1;
};

void StreamBatchCb(int batch_index, int total_batches, const char *batch_json, void *userdata) {
  auto *a = static_cast<StreamAccum *>(userdata);
  // batch_json is valid only for the callback's duration — parse before locking/returning.
  json parsed;
  try {
    parsed = json::parse(batch_json);
  } catch (...) {
    return;
  }
  std::lock_guard<std::mutex> lock(a->mu);
  a->total_batches = total_batches;
  a->by_index[batch_index] = std::move(parsed);
}

// Compute VNote's drain-pool size: N = hardware_concurrency(); if 0 → 2; N = min(N, 8).
unsigned DrainPoolSize() {
  unsigned n = std::thread::hardware_concurrency();
  if (n == 0) n = 2;
  n = std::min(n, 8u);
  return n;
}

// File-boundary maxResults truncation over the reassembled matched-file array, byte-identical to
// SimpleSearchBackend::Search / VNote's SearchWorker::applyMaxResultsTruncation. No-op if <= 0.
// Returns true if it actually removed any matches (i.e., the cap was hit).
bool ApplyMaxResultsTruncation(std::vector<json> *files, int max_results) {
  if (max_results <= 0) return false;
  int total = 0;
  for (size_t i = 0; i < files->size(); ++i) {
    json &file_obj = (*files)[i];
    if (!file_obj.contains("matches") || !file_obj["matches"].is_array()) continue;
    json &occ = file_obj["matches"];
    for (size_t j = 0; j < occ.size(); ++j) {
      ++total;
      if (total >= max_results) {
        const bool cut = (j + 1 < occ.size()) || (i + 1 < files->size());
        occ.erase(occ.begin() + static_cast<long>(j) + 1, occ.end());
        if (file_obj.contains("matchCount")) file_obj["matchCount"] = occ.size();
        files->erase(files->begin() + static_cast<long>(i) + 1, files->end());
        return cut;
      }
    }
  }
  return false;
}

struct LineHit {
  std::string display;  // canonical relpath for reporting
  std::string text;     // line text (context / non-ASCII tagging)
};

using LineId = std::pair<std::string, int>;  // (KeyOf(relpath), lineNumber)

struct VNoteResult {
  std::set<LineId> lines;
  std::map<LineId, LineHit> hits;
  std::set<std::string> file_keys;
  unsigned drain_threads = 0;
  bool truncated = false;  // true when --max-results actually capped the vxcore result set
};

VNoteResult RunVNoteSearch(VxCoreContextHandle ctx, const std::string &notebook_id,
                           const Options &opts) {
  VNoteResult result;

  json scope;
  scope["folderPath"] = opts.folder;
  scope["recursive"] = true;

  json query;
  query["pattern"] = opts.pattern;
  query["caseSensitive"] = opts.case_sensitive;
  query["wholeWord"] = opts.word;
  query["regex"] = opts.regex;
  query["maxResults"] = opts.max_results;
  query["scope"] = scope;
  const std::string query_json = query.dump();

  StreamAccum accum;
  std::atomic<bool> done{false};
  volatile int cancel_flag = 0;

  const unsigned pool = DrainPoolSize();
  result.drain_threads = pool;

  std::vector<std::thread> drain_threads;
  drain_threads.reserve(pool);
  for (unsigned i = 0; i < pool; ++i) {
    drain_threads.emplace_back([ctx, &done]() {
      while (!done.load(std::memory_order_relaxed)) {
        vxcore_work_queue_process_next(ctx, "vxcore.search", 100);
      }
    });
  }

  VxCoreError search_err = VXCORE_OK;
  std::thread search_thread([&]() {
    search_err =
        vxcore_search_content_streaming(ctx, notebook_id.c_str(), query_json.c_str(), nullptr,
                                        /*batch_size=*/0, StreamBatchCb, &accum, &cancel_flag);
  });

  search_thread.join();  // streaming returns only after all work is drained
  done.store(true, std::memory_order_relaxed);
  for (auto &t : drain_threads) {
    if (t.joinable()) t.join();
  }

  if (search_err != VXCORE_OK) {
    std::cerr << "warning: streaming search returned " << vxcore_error_message(search_err) << "\n";
  }

  // Reassemble matched files in ascending batch_index order.
  std::vector<json> matched_files;
  for (const auto &kv : accum.by_index) {
    const json &batch = kv.second;
    if (!batch.contains("matches") || !batch["matches"].is_array()) continue;
    for (const auto &mf : batch["matches"]) matched_files.push_back(mf);
  }

  result.truncated = ApplyMaxResultsTruncation(&matched_files, opts.max_results);

  for (const auto &mf : matched_files) {
    if (!mf.contains("path") || !mf["path"].is_string()) continue;
    const std::string rel = CanonRel(mf["path"].get<std::string>());
    const std::string key = KeyOf(rel);
    result.file_keys.insert(key);
    if (!mf.contains("matches") || !mf["matches"].is_array()) continue;
    for (const auto &m : mf["matches"]) {
      if (!m.contains("lineNumber") || !m["lineNumber"].is_number_integer()) continue;
      const int line = m["lineNumber"].get<int>();
      const LineId id{key, line};
      result.lines.insert(id);
      LineHit hit;
      hit.display = rel;
      if (m.contains("lineText") && m["lineText"].is_string())
        hit.text = m["lineText"].get<std::string>();
      result.hits.emplace(id, std::move(hit));
    }
  }

  return result;
}

// ============================================================================
// rg runner + JSON parse.
// ============================================================================

struct RgHit {
  std::string rel;  // canonical notebook-relative path
  int line;
  std::string text;  // matched line text
};

// Convert an rg absolute path (UTF-8) to a notebook-relative path by stripping the notebook root
// prefix (case-insensitively on Windows). Falls back to a lexical relative if prefixes disagree.
std::string AbsToRel(const std::string &abs_utf8, const std::string &root_norm) {
  std::string abs_norm = abs_utf8;
  for (char &c : abs_norm) {
    if (c == '\\') c = '/';
  }
  std::string cmp_abs = IsWindows() ? ToLowerAscii(abs_norm) : abs_norm;
  std::string cmp_root = IsWindows() ? ToLowerAscii(root_norm) : root_norm;
  if (!cmp_root.empty() && cmp_abs.rfind(cmp_root + "/", 0) == 0) {
    return CanonRel(abs_norm.substr(root_norm.size() + 1));
  }
  // Fallback: lexical relative on real paths (purely lexical; no filesystem access).
  auto rel = std::filesystem::path(Utf8ToPath(abs_utf8)).lexically_relative(Utf8ToPath(root_norm));
  return CanonRel(PathToUtf8(rel));
}

std::vector<RgHit> RunRg(const Options &opts, const std::string &scope_abs_utf8,
                         const std::string &root_norm, int *rg_exit) {
  std::vector<RgHit> hits;

  std::vector<std::string> args;
  args.push_back("rg");
  args.push_back("--json");
  args.push_back("--no-ignore");
  args.push_back("--hidden");
  args.push_back("--text");

  if (opts.case_sensitive) {
    args.push_back("-s");
  } else {
    // ASCII byte-folding to match the simple backend's std::tolower path.
    args.push_back("--ignore-case");
    args.push_back("--no-unicode");
  }
  if (opts.word) args.push_back("--word-regexp");
  if (!opts.regex) args.push_back("--fixed-strings");

  args.push_back("-e");
  args.push_back(opts.pattern);
  args.push_back(scope_abs_utf8);

  ProcResult pr = RunCapture(args);
  *rg_exit = pr.exit_code;
  if (!pr.launched) {
    std::cerr << "error: failed to launch rg\n";
    return hits;
  }
  // rg: exit 0 = matches, 1 = no matches, 2 = error.
  if (pr.exit_code == 2) {
    std::cerr << "warning: rg reported an error (exit 2); results may be incomplete\n";
  }

  std::istringstream stream(pr.out);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty()) continue;
    json rec;
    try {
      rec = json::parse(line);
    } catch (...) {
      continue;
    }
    if (!rec.contains("type") || rec["type"] != "match") continue;
    if (!rec.contains("data") || !rec["data"].is_object()) continue;
    const json &data = rec["data"];

    if (!data.contains("path") || !data["path"].is_object()) continue;
    const json &path = data["path"];
    if (!path.contains("text") || !path["text"].is_string()) continue;  // skip non-UTF-8 paths
    const std::string abs_path = path["text"].get<std::string>();

    if (!data.contains("line_number") || !data["line_number"].is_number_integer()) continue;
    const int line_number = data["line_number"].get<int>();

    std::string text;
    if (data.contains("lines") && data["lines"].is_object() && data["lines"].contains("text") &&
        data["lines"]["text"].is_string()) {
      text = data["lines"]["text"].get<std::string>();
    }

    RgHit hit;
    hit.rel = AbsToRel(abs_path, root_norm);
    hit.line = line_number;
    hit.text = text;
    hits.push_back(std::move(hit));
  }

  return hits;
}

// ============================================================================
// Classification, diff, report.
// ============================================================================

struct Discrepancy {
  std::string display;
  int line;
  std::string text;
  bool diagnostic;
};

bool IsDiagnosticMode(const Options &opts) { return opts.word || opts.regex; }

// Route a single discrepancy on line |non_ascii| to diagnostic vs pass/fail per the fidelity
// policy. |truncated| is true when --max-results actually capped the vxcore result set: rg is
// NOT capped, so beyond-cap matches would otherwise show as spurious pass/fail "missed" — under
// truncation the whole run degrades to diagnostic-only.
bool DiscrepancyIsDiagnostic(const Options &opts, bool line_non_ascii, bool truncated) {
  if (truncated) return true;
  if (IsDiagnosticMode(opts)) return true;
  if (!opts.case_sensitive && line_non_ascii) return true;  // Unicode vs ASCII case-fold
  return false;
}

std::string ModeBanner(const Options &opts) {
  std::string m = opts.regex ? "regex" : "literal";
  m += opts.case_sensitive ? ", case-sensitive" : ", case-insensitive";
  if (opts.word) m += ", whole-word";
  return m;
}

std::string FidelityBanner(const Options &opts, bool truncated) {
  if (truncated) {
    return "DIAGNOSTIC-ONLY (--max-results capped vxcore but not rg; comparison is not pass/fail)";
  }
  if (IsDiagnosticMode(opts)) {
    return opts.regex ? "DIAGNOSTIC-ONLY (regex dialect: std::regex vs rg)"
                      : "DIAGNOSTIC-ONLY (whole-word: '_' boundary rule differs)";
  }
  if (!opts.case_sensitive) {
    return "PASS/FAIL for ASCII lines; non-ASCII lines are DIAGNOSTIC-ONLY";
  }
  return "PASS/FAIL (literal, case-sensitive: exact byte agreement)";
}

}  // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
  // The narrow CRT argv is decoded in the active ANSI code page, NOT UTF-8, so non-ASCII
  // notebook roots / scopes / patterns would be corrupted before the UTF-8 helpers run.
  // Re-acquire the arguments from the wide command line and re-encode them as UTF-8.
  std::vector<std::string> utf8_storage;
  std::vector<char *> utf8_argv;
  {
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
      utf8_storage.reserve(static_cast<size_t>(wargc));
      for (int i = 0; i < wargc; ++i) utf8_storage.push_back(WideToUtf8(wargv[i]));
      LocalFree(wargv);
      utf8_argv.reserve(utf8_storage.size());
      for (auto &s : utf8_storage) utf8_argv.push_back(const_cast<char *>(s.c_str()));
      argc = wargc;
      argv = utf8_argv.data();
    }
  }
#endif

  Options opts;
  std::string parse_err;
  if (!ParseArgs(argc, argv, &opts, &parse_err)) {
    std::cerr << "error: " << parse_err << "\n\n";
    PrintUsage();
    return 2;
  }
  if (opts.help) {
    PrintUsage();
    return 0;
  }

  // Normalize the scope folder once, matching vxcore's scope.folderPath normalization, so the
  // indexed-set enumeration, search results, and rg prefix-stripping all share identical keys.
  opts.folder = CanonRel(opts.folder);
  if (opts.folder.empty()) opts.folder = ".";

  // --- Preflight ---
  const std::string root = StripTrailingSep(opts.notebook_root);
  if (!Utf8PathExists(root)) {
    std::cerr << "fatal: notebook root not found: " << root << "\n";
    return 2;
  }
  if (!Utf8PathExists(root + "/vx_notebook/config.json")) {
    std::cerr << "fatal: not a bundled notebook (missing vx_notebook/config.json): " << root
              << "\n";
    return 2;
  }
  {
    int ver_exit = -1;
    ProcResult ver = RunCapture({"rg", "--version"});
    ver_exit = ver.exit_code;
    if (!ver.launched || ver_exit != 0) {
      std::cerr << "fatal: ripgrep (rg) not found on PATH — comparison impossible.\n";
      return 2;
    }
  }

  // Resolve absolute paths for rg + prefix stripping.
  std::error_code ec;
  std::filesystem::path root_abs = std::filesystem::weakly_canonical(Utf8ToPath(root), ec);
  if (ec || root_abs.empty()) {
    root_abs = std::filesystem::absolute(Utf8ToPath(root), ec);
  }
  const std::string root_norm = StripTrailingSep([&] {
    std::string s = PathToUtf8(root_abs);
    for (char &c : s) {
      if (c == '\\') c = '/';
    }
    return s;
  }());

  std::filesystem::path scope_abs = root_abs;
  if (opts.folder != ".") {
    scope_abs = root_abs / Utf8ToPath(opts.folder);
  }
  const std::string scope_abs_utf8 = PathToUtf8(scope_abs);

  // --- Context + notebook open ---
  vxcore_set_test_mode(1);  // isolate: never touch the user's real vxcore.json / session
  if (!opts.verbose) {
    // Test mode shares %TEMP%\vxcore_test_config with vxcore's own test suite, whose session
    // may list many stale notebooks; suppress the resulting stderr spam unless --verbose.
    vxcore_log_enable_console(0);
  }
  vxcore_set_app_info("VNoteX", "vxcore");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  if (err != VXCORE_OK || !ctx) {
    std::cerr << "fatal: vxcore_context_create failed: " << vxcore_error_message(err) << "\n";
    return 2;
  }

  // Pin the search backend. CreateSearchManager picks the first of search.backends that is
  // "simple" or ("rg" and available), so ["rg","simple"] prefers rg with a simple fallback.
  {
    json cfg;
    if (opts.backend == "rg") {
      cfg["search"]["backends"] = json::array({"rg", "simple"});
    } else {
      cfg["search"]["backends"] = json::array({"simple"});
    }
    VxCoreError cfg_err = vxcore_context_update_config(ctx, cfg.dump().c_str());
    if (cfg_err != VXCORE_OK) {
      std::cerr << "warning: failed to pin backend (" << vxcore_error_message(cfg_err)
                << "); proceeding with default\n";
    }
  }

  char *notebook_id_c = nullptr;
  err = vxcore_notebook_open(ctx, root.c_str(), &notebook_id_c);
  if (err != VXCORE_OK || !notebook_id_c) {
    std::cerr << "fatal: vxcore_notebook_open failed: " << vxcore_error_message(err) << "\n";
    if (notebook_id_c) vxcore_string_free(notebook_id_c);
    vxcore_context_destroy(ctx);
    return 2;
  }
  const std::string notebook_id(notebook_id_c);
  vxcore_string_free(notebook_id_c);

  // --- Enumerate indexed set ---
  IndexedSet indexed;
  EnumerateFolder(ctx, notebook_id, opts.folder, &indexed);

  // --- Run VNote-faithful streaming search ---
  VNoteResult vres = RunVNoteSearch(ctx, notebook_id, opts);

  // --- Run rg ground truth ---
  int rg_exit = -1;
  std::vector<RgHit> rg_hits = RunRg(opts, scope_abs_utf8, root_norm, &rg_exit);

  // --- Classify rg hits ---
  std::set<LineId> r_idx;  // rg hits in indexed files
  std::map<LineId, LineHit> r_idx_hits;
  std::map<std::string, std::vector<RgHit>> discovery;  // key → rg hits in un-indexed files
  int infra_lines = 0;
  int rg_total_lines = static_cast<int>(rg_hits.size());

  for (const auto &h : rg_hits) {
    const std::string key = KeyOf(h.rel);
    if (IsInfrastructure(key)) {
      ++infra_lines;
      continue;
    }
    if (indexed.file_keys.count(key)) {
      const LineId id{key, h.line};
      r_idx.insert(id);
      LineHit lh;
      lh.display = h.rel;
      lh.text = h.text;
      r_idx_hits.emplace(id, std::move(lh));
    } else {
      discovery[key].push_back(h);
    }
  }

  // --- Diff ---
  std::vector<Discrepancy> missed;  // R_idx − V
  for (const auto &id : r_idx) {
    if (vres.lines.count(id)) continue;
    const auto &lh = r_idx_hits[id];
    const bool na = HasNonAscii(lh.text);
    missed.push_back(
        {lh.display, id.second, lh.text, DiscrepancyIsDiagnostic(opts, na, vres.truncated)});
  }

  std::vector<Discrepancy> false_pos;  // V − R_idx
  for (const auto &id : vres.lines) {
    if (r_idx.count(id)) continue;
    auto it = vres.hits.find(id);
    const std::string disp = it != vres.hits.end() ? it->second.display : id.first;
    const std::string text = it != vres.hits.end() ? it->second.text : std::string();
    const bool na = HasNonAscii(text);
    false_pos.push_back({disp, id.second, text, DiscrepancyIsDiagnostic(opts, na, vres.truncated)});
  }

  auto count_split = [](const std::vector<Discrepancy> &v, int *pass_fail, int *diag) {
    *pass_fail = 0;
    *diag = 0;
    for (const auto &d : v) {
      if (d.diagnostic)
        ++*diag;
      else
        ++*pass_fail;
    }
  };
  int missed_pf = 0, missed_diag = 0, fp_pf = 0, fp_diag = 0;
  count_split(missed, &missed_pf, &missed_diag);
  count_split(false_pos, &fp_pf, &fp_diag);

  int disc_files = static_cast<int>(discovery.size());
  int disc_lines = 0;
  for (const auto &kv : discovery) disc_lines += static_cast<int>(kv.second.size());

  const int pass_fail_total = missed_pf + fp_pf;
  const int diag_total = missed_diag + fp_diag;

  // --- Exit code ---
  int exit_code = 0;
  if (pass_fail_total > 0) exit_code = 1;
  if (opts.strict && (pass_fail_total > 0 || diag_total > 0 || disc_files > 0)) exit_code = 1;

  // --- Report ---
  if (opts.json_out) {
    json out;
    out["notebook"] = root;
    out["scopeFolder"] = opts.folder;
    out["pattern"] = opts.pattern;
    out["mode"] = ModeBanner(opts);
    out["backend"] = opts.backend;
    out["drainThreads"] = vres.drain_threads;
    out["truncated"] = vres.truncated;
    out["indexedFiles"] = indexed.file_count;
    out["phantomFolders"] = indexed.phantom_folders;
    out["fidelity"] = FidelityBanner(opts, vres.truncated);
    out["vxcoreLines"] = static_cast<int>(vres.lines.size());
    out["vxcoreFiles"] = static_cast<int>(vres.file_keys.size());
    out["rgTotalLines"] = rg_total_lines;
    out["rgIndexedLines"] = static_cast<int>(r_idx.size());
    out["infraLines"] = infra_lines;
    out["rgExit"] = rg_exit;

    auto dump_bucket = [](const std::vector<Discrepancy> &v) {
      json arr = json::array();
      for (const auto &d : v) {
        json o;
        o["path"] = d.display;
        o["line"] = d.line;
        o["diagnostic"] = d.diagnostic;
        o["text"] = TrimForDisplay(d.text);
        arr.push_back(std::move(o));
      }
      return arr;
    };
    out["missed"] = {
        {"passFail", missed_pf}, {"diagnostic", missed_diag}, {"items", dump_bucket(missed)}};
    out["falsePositives"] = {
        {"passFail", fp_pf}, {"diagnostic", fp_diag}, {"items", dump_bucket(false_pos)}};

    json disc_arr = json::array();
    for (const auto &kv : discovery) {
      json o;
      o["path"] = kv.second.empty() ? kv.first : CanonRel(kv.second.front().rel);
      o["lines"] = static_cast<int>(kv.second.size());
      disc_arr.push_back(std::move(o));
    }
    out["discovery"] = {{"files", disc_files}, {"lines", disc_lines}, {"items", disc_arr}};
    out["passFailDiscrepancies"] = pass_fail_total;
    out["result"] = exit_code == 0 ? "PASS" : "FAIL";
    std::cout << out.dump(2) << "\n";
    vxcore_context_destroy(ctx);
    return exit_code;
  }

  auto print_bucket = [&](const std::string &title, const std::vector<Discrepancy> &v, int pf,
                          int diag) {
    std::cout << "\n--- " << title << " ---\n";
    std::cout << "  pass/fail:  " << pf << "\n";
    std::cout << "  diagnostic: " << diag << "\n";
    int shown = 0;
    for (const auto &d : v) {
      if (shown >= opts.limit) {
        std::cout << "  ... (" << (static_cast<int>(v.size()) - shown) << " more)\n";
        break;
      }
      std::cout << "  [" << (d.diagnostic ? "diag" : "P/F ") << "] " << d.display << ":" << d.line
                << ": " << TrimForDisplay(d.text) << "\n";
      ++shown;
    }
  };

  std::cout << "=== vxcore Content-Search Validation ===\n";
  std::cout << "Notebook:      " << root << "\n";
  std::cout << "Scope folder:  " << opts.folder << "\n";
  std::cout << "Pattern:       \"" << opts.pattern << "\"\n";
  std::cout << "Mode:          " << ModeBanner(opts) << "\n";
  std::cout << "Backend:       " << opts.backend << " (pinned)\n";
  std::cout << "Drain threads: " << vres.drain_threads << "\n";
  std::cout << "Indexed files: " << indexed.file_count;
  if (!indexed.phantom_folders.empty())
    std::cout << "   (phantom folders: " << indexed.phantom_folders.size() << ")";
  std::cout << "\n";
  std::cout << "Fidelity:      " << FidelityBanner(opts, vres.truncated) << "\n";
  std::cout << "\n";
  std::cout << "vxcore matches:      " << vres.lines.size() << " lines across "
            << vres.file_keys.size() << " files\n";
  std::cout << "rg matches (total):  " << rg_total_lines << " lines\n";
  std::cout << "  in indexed files:  " << r_idx.size() << " (R_idx)\n";
  std::cout << "  infrastructure:    " << infra_lines << " (ignored: vx_notebook/, .git/)\n";
  std::cout << "  un-indexed:        " << disc_lines << " lines across " << disc_files
            << " files (discovery)\n";

  print_bucket("Missed matches (rg matched, vxcore did not; indexed files)", missed, missed_pf,
               missed_diag);
  print_bucket("False positives (vxcore matched, rg did not; indexed files)", false_pos, fp_pf,
               fp_diag);

  std::cout << "\n--- Discovery gaps (on-disk matches in un-indexed files; expected) ---\n";
  std::cout << "  files: " << disc_files << ", lines: " << disc_lines << "\n";
  {
    int shown = 0;
    for (const auto &kv : discovery) {
      if (shown >= opts.limit) {
        std::cout << "  ... (" << (disc_files - shown) << " more files)\n";
        break;
      }
      const std::string disp = kv.second.empty() ? kv.first : CanonRel(kv.second.front().rel);
      std::cout << "  " << disp << "  (" << kv.second.size() << " lines)\n";
      ++shown;
    }
  }

  std::cout << "\n";
  if (exit_code == 0) {
    std::cout << "RESULT: PASS";
    if (diag_total > 0 || disc_files > 0) {
      std::cout << " (with " << diag_total << " diagnostic + " << disc_files
                << " discovery warnings)";
    }
    std::cout << "\n";
  } else {
    std::cout << "RESULT: FAIL — " << pass_fail_total << " pass/fail discrepancies";
    if (opts.strict && pass_fail_total == 0) {
      std::cout << " (strict: diagnostic/discovery buckets non-empty)";
    }
    std::cout << "\n";
  }

  vxcore_context_destroy(ctx);
  return exit_code;
}
