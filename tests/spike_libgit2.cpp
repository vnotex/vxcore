// Phase-0 build spike for libgit2 (Task T1 of git-sync-backend plan).
//
// Purpose: prove that the vendored libgit2 builds, links, and supports the
// separate gitdir/workdir layout we depend on for VNote sync. Specifically
// validates `git_repository_init_ext` with `workdir_path` pointing at the
// notebook root and the gitdir pointing at `<workdir>/.vxsync/`, which is the
// layout the future GitSyncBackend will use (`vx_notebook/vx_sync/`). Also
// performs a single end-to-end commit so we know the index/tree/commit pipeline
// works on every supported platform with our static-link configuration.
//
// On success this prints exactly the line `spike_libgit2 passed` so the QA
// scenarios can grep for it.

#include <git2.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// Helper: print libgit2's last error and return 1 from main.
int FailHere(const char* step) {
  const git_error* e = git_error_last();
  std::fprintf(stderr, "spike_libgit2 FAILED at %s: %s\n", step,
               (e && e->message) ? e->message : "unknown");
  return 1;
}

// Cross-platform temp-dir resolution. Mirrors the contract documented in T1.
std::string ResolveTempDir() {
  const char* tmp = std::getenv("TEMP");
  if (!tmp || !*tmp) tmp = std::getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = std::getenv("TMP");
#ifdef _WIN32
  if (!tmp || !*tmp) tmp = "C:\\Windows\\Temp";
#else
  if (!tmp || !*tmp) tmp = "/tmp";
#endif
  return std::string(tmp);
}

}  // namespace

int main() {
  const int init_count = git_libgit2_init();
  if (init_count < 1) {
    return FailHere("git_libgit2_init");
  }

  const std::string workdir = ResolveTempDir() + "/vxcore_spike_libgit2_workdir";
  const std::string gitdir = workdir + "/.vxsync";
  const std::string hello_path = workdir + "/hello.txt";

  // Clean up any leftovers from a previous run so init can succeed cleanly.
  try {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(workdir), ec);
    (void)ec;  // best-effort
  } catch (const std::exception& e) {
    std::fprintf(stderr, "spike_libgit2 WARN: pre-clean failed: %s\n", e.what());
  }

  git_repository* repo = nullptr;
  git_index* idx = nullptr;
  git_tree* tree = nullptr;
  git_signature* sig = nullptr;

  // Configure init options: gitdir = first arg to git_repository_init_ext,
  // workdir explicitly set to the notebook root via opts.workdir_path.
  git_repository_init_options opts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
  if (git_repository_init_options_init(&opts,
                                       GIT_REPOSITORY_INIT_OPTIONS_VERSION) < 0) {
    git_libgit2_shutdown();
    return FailHere("git_repository_init_options_init");
  }
  opts.flags = GIT_REPOSITORY_INIT_MKDIR | GIT_REPOSITORY_INIT_MKPATH |
               GIT_REPOSITORY_INIT_NO_REINIT;
  opts.workdir_path = workdir.c_str();
  opts.initial_head = "main";

  if (git_repository_init_ext(&repo, gitdir.c_str(), &opts) < 0) {
    git_libgit2_shutdown();
    return FailHere("git_repository_init_ext");
  }

  // Verify workdir is the notebook root, NOT the gitdir.
  const char* reported_workdir = git_repository_workdir(repo);
  if (!reported_workdir) {
    git_repository_free(repo);
    git_libgit2_shutdown();
    return FailHere("git_repository_workdir returned null");
  }
  // libgit2 returns paths with a trailing slash; compare against the prefix.
  std::string reported(reported_workdir);
  std::string expected_prefix = workdir;
  // Normalize backslashes so the comparison is portable on Windows.
  for (auto& c : reported) {
    if (c == '\\') c = '/';
  }
  for (auto& c : expected_prefix) {
    if (c == '\\') c = '/';
  }
  if (reported.find(expected_prefix) != 0) {
    std::fprintf(stderr,
                 "spike_libgit2 FAILED: workdir mismatch: got '%s', expected "
                 "prefix '%s'\n",
                 reported.c_str(), expected_prefix.c_str());
    git_repository_free(repo);
    git_libgit2_shutdown();
    return 1;
  }

  // Write a small file inside the workdir.
  try {
    std::ofstream ofs(std::filesystem::u8path(hello_path),
                      std::ios::out | std::ios::trunc | std::ios::binary);
    if (!ofs) {
      std::fprintf(stderr,
                   "spike_libgit2 FAILED: cannot open hello.txt for write\n");
      git_repository_free(repo);
      git_libgit2_shutdown();
      return 1;
    }
    ofs << "hi";
  } catch (const std::exception& e) {
    std::fprintf(stderr, "spike_libgit2 FAILED: ofstream threw: %s\n",
                 e.what());
    git_repository_free(repo);
    git_libgit2_shutdown();
    return 1;
  }

  // git add hello.txt
  if (git_repository_index(&idx, repo) < 0) {
    git_repository_free(repo);
    git_libgit2_shutdown();
    return FailHere("git_repository_index");
  }
  if (git_index_add_bypath(idx, "hello.txt") < 0) {
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return FailHere("git_index_add_bypath");
  }
  if (git_index_write(idx) < 0) {
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return FailHere("git_index_write");
  }

  git_oid tree_oid;
  if (git_index_write_tree(&tree_oid, idx) < 0) {
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return FailHere("git_index_write_tree");
  }

  if (git_signature_now(&sig, "Spike", "spike@test") < 0) {
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return FailHere("git_signature_now");
  }

  if (git_tree_lookup(&tree, repo, &tree_oid) < 0) {
    git_signature_free(sig);
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return FailHere("git_tree_lookup");
  }

  git_oid commit_oid;
  if (git_commit_create_v(&commit_oid, repo, "HEAD", sig, sig, nullptr,
                          "spike commit", tree, 0) < 0) {
    git_tree_free(tree);
    git_signature_free(sig);
    git_index_free(idx);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return FailHere("git_commit_create_v");
  }

  git_tree_free(tree);
  git_signature_free(sig);
  git_index_free(idx);
  git_repository_free(repo);
  git_libgit2_shutdown();

  std::printf("workdir: %s\n", workdir.c_str());
  std::printf("gitdir: %s\n", gitdir.c_str());
  std::printf("spike_libgit2 passed\n");
  return 0;
}
