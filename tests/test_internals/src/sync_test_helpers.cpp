#include "test_internals/sync_test_helpers.h"

#include <git2.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "utils/file_utils.h"

namespace vxcore_test_internals {

namespace {

std::string PathToFileUrl(const std::string& abs_path) {
  // libgit2 accepts file:///C:/... on Windows and file:///abs/... on POSIX.
  // We normalize to forward slashes and prepend file:/// (three slashes).
  std::string normalized = abs_path;
  for (auto& c : normalized) {
    if (c == '\\') c = '/';
  }
  if (!normalized.empty() && normalized.front() == '/') {
    return "file://" + normalized;  // POSIX absolute already starts with /
  }
  return "file:///" + normalized;
}

void ThrowLibgit2(const char* op) {
  const git_error* e = git_error_last();
  std::ostringstream oss;
  oss << op << " failed: "
      << (e && e->message ? e->message : "(no libgit2 message)");
  throw std::runtime_error(oss.str());
}

}  // namespace

std::string MakeBareRemote(const std::string& bare_path) {
  std::error_code ec;
  std::filesystem::remove_all(vxcore::PathFromUtf8(bare_path), ec);
  // ec ignored: missing path is fine, real errors will surface on init.
  std::filesystem::create_directories(
      vxcore::PathFromUtf8(bare_path).parent_path(), ec);

  git_repository_init_options opts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
  opts.flags = GIT_REPOSITORY_INIT_MKPATH | GIT_REPOSITORY_INIT_BARE;
  opts.initial_head = "main";

  git_repository* repo = nullptr;
  if (git_repository_init_ext(&repo, bare_path.c_str(), &opts) != 0) {
    ThrowLibgit2("MakeBareRemote: git_repository_init_ext");
  }
  git_repository_free(repo);

  return PathToFileUrl(bare_path);
}

std::string MakeTempWorkspace() {
  static std::atomic<unsigned> counter{0};
  const auto base = std::filesystem::temp_directory_path();
  const auto stamp =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  std::ostringstream name;
  name << "vxcore_test_ws_" << stamp << "_" << counter.fetch_add(1);
  auto dir = base / name.str();

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    throw std::runtime_error("MakeTempWorkspace: create_directories: " +
                             ec.message());
  }
  return vxcore::PathToUtf8(dir);
}

git_signature* CreateTestSignature() {
  git_signature* sig = nullptr;
  if (git_signature_now(&sig, "VNote Test", "vnote-test@vnote.local") != 0) {
    return nullptr;
  }
  return sig;
}

int RunGitCommand(const std::vector<std::string>& args) {
  if (args.empty()) {
    return GIT_ERROR;
  }
  if (args[0] == "init") {
    bool bare = false;
    std::string path;
    for (std::size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--bare") {
        bare = true;
      } else {
        path = args[i];
      }
    }
    if (path.empty()) return GIT_ERROR;

    git_repository_init_options opts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
    opts.flags = GIT_REPOSITORY_INIT_MKPATH;
    if (bare) opts.flags |= GIT_REPOSITORY_INIT_BARE;
    opts.initial_head = "main";

    git_repository* repo = nullptr;
    int rc = git_repository_init_ext(&repo, path.c_str(), &opts);
    if (repo) git_repository_free(repo);
    return rc;
  }
  return GIT_ERROR;
}

}  // namespace vxcore_test_internals
