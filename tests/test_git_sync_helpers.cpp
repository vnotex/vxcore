// libgit2-based test fixture helpers (Task T10 of git-sync-backend plan).
//
// These helpers are linked into git_sync_test_helpers (a STATIC library) and
// consumed by all test_git_sync_* binaries plus test_sync_manager_dispatch.
// Each public function constructs its OWN LibGit2Init RAII guard so callers
// don't need to remember to initialize libgit2.

#include "test_git_sync_helpers.h"

#include <git2.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

#include "sync/git/libgit2_init.h"

namespace vxcore_test {

namespace {

[[noreturn]] void throw_git_error(const char *step) {
  const git_error *e = git_error_last();
  std::string msg = std::string("[helper] ") + step + ": " +
                    ((e && e->message) ? e->message : "unknown");
  throw std::runtime_error(msg);
}

void check(int rc, const char *step) {
  if (rc != 0) throw_git_error(step);
}

std::string to_forward_slashes(std::string s) {
  std::replace(s.begin(), s.end(), '\\', '/');
  return s;
}

std::string make_file_url(const std::string &abs_path) {
  std::string p = to_forward_slashes(abs_path);
  // Build "file:///<abs_path>". On Unix abs paths start with '/', so result
  // becomes "file:////tmp/foo" — collapse to "file:///tmp/foo".
  if (!p.empty() && p.front() == '/') {
    return "file://" + p;
  }
  return "file:///" + p;
}

std::string random_suffix() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dist;
  std::ostringstream oss;
  oss << std::hex << dist(gen);
  return oss.str();
}

}  // namespace

std::string create_bare_repo(const std::string &bare_path) {
  vxcore::LibGit2Init guard;

  // Best-effort cleanup of any leftover repo at this path.
  {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(bare_path), ec);
    (void)ec;
  }

  git_repository_init_options opts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
  check(git_repository_init_options_init(&opts,
                                         GIT_REPOSITORY_INIT_OPTIONS_VERSION),
        "git_repository_init_options_init");
  opts.flags = GIT_REPOSITORY_INIT_MKDIR | GIT_REPOSITORY_INIT_MKPATH |
               GIT_REPOSITORY_INIT_BARE;
  opts.initial_head = "main";

  git_repository *repo = nullptr;
  check(git_repository_init_ext(&repo, bare_path.c_str(), &opts),
        "git_repository_init_ext(bare)");
  git_repository_free(repo);

  // Compute absolute path for URL construction.
  std::error_code ec;
  std::filesystem::path abs =
      std::filesystem::absolute(std::filesystem::u8path(bare_path), ec);
  std::string abs_str = ec ? bare_path : abs.string();
  return make_file_url(abs_str);
}

void commit_file(const std::string &bare_repo_path, const std::string &rel_file,
                 const std::string &content, const std::string &commit_msg) {
  vxcore::LibGit2Init guard;

  // Build a temp clone path.
  std::filesystem::path tmp_root = std::filesystem::temp_directory_path();
  std::string clone_dir =
      "vxcore_helper_clone_" + random_suffix();
  std::filesystem::path clone_path = tmp_root / clone_dir;

  {
    std::error_code ec;
    std::filesystem::remove_all(clone_path, ec);
    (void)ec;
  }

  // Compute absolute bare path for file:// URL.
  std::error_code ec;
  std::filesystem::path bare_abs =
      std::filesystem::absolute(std::filesystem::u8path(bare_repo_path), ec);
  std::string bare_abs_str = ec ? bare_repo_path : bare_abs.string();
  std::string url = make_file_url(bare_abs_str);

  git_repository *repo = nullptr;
  git_index *idx = nullptr;
  git_tree *tree = nullptr;
  git_signature *sig = nullptr;
  git_remote *remote = nullptr;
  git_commit *parent_commit = nullptr;
  git_object *parent_obj = nullptr;

  auto cleanup = [&]() {
    if (parent_obj) git_object_free(parent_obj);
    if (parent_commit) git_commit_free(parent_commit);
    if (remote) git_remote_free(remote);
    if (sig) git_signature_free(sig);
    if (tree) git_tree_free(tree);
    if (idx) git_index_free(idx);
    if (repo) git_repository_free(repo);
    std::error_code ec2;
    std::filesystem::remove_all(clone_path, ec2);
    (void)ec2;
  };

  try {
    // Init an empty non-bare repo, then add the bare as origin remote.
    // Avoids git_clone over file:// (which has portability issues on Windows
    // with the local transport).
    git_repository_init_options iopts = GIT_REPOSITORY_INIT_OPTIONS_INIT;
    int rc = git_repository_init_options_init(
        &iopts, GIT_REPOSITORY_INIT_OPTIONS_VERSION);
    if (rc != 0) {
      cleanup();
      throw_git_error("git_repository_init_options_init(clone)");
    }
    iopts.flags = GIT_REPOSITORY_INIT_MKDIR | GIT_REPOSITORY_INIT_MKPATH;
    iopts.initial_head = "main";
    rc = git_repository_init_ext(&repo, clone_path.string().c_str(), &iopts);
    if (rc != 0) {
      cleanup();
      throw_git_error("git_repository_init_ext(clone-workdir)");
    }

    git_remote *origin_create = nullptr;
    rc = git_remote_create(&origin_create, repo, "origin", url.c_str());
    if (rc != 0) {
      cleanup();
      throw_git_error("git_remote_create(origin)");
    }
    git_remote_free(origin_create);

    // Write the file (create parent dirs as needed).
    std::filesystem::path file_full = clone_path / rel_file;
    std::error_code mec;
    std::filesystem::create_directories(file_full.parent_path(), mec);
    {
      std::ofstream ofs(file_full,
                        std::ios::out | std::ios::trunc | std::ios::binary);
      if (!ofs) {
        cleanup();
        throw std::runtime_error(std::string("[helper] open file for write: ") +
                                 file_full.string());
      }
      ofs << content;
    }

    // git add <rel_file>
    rc = git_repository_index(&idx, repo);
    if (rc != 0) {
      cleanup();
      throw_git_error("git_repository_index");
    }
    // libgit2 needs forward-slash relative paths.
    std::string rel_norm = to_forward_slashes(rel_file);
    rc = git_index_add_bypath(idx, rel_norm.c_str());
    if (rc != 0) {
      cleanup();
      throw_git_error("git_index_add_bypath");
    }
    rc = git_index_write(idx);
    if (rc != 0) {
      cleanup();
      throw_git_error("git_index_write");
    }

    git_oid tree_oid;
    rc = git_index_write_tree(&tree_oid, idx);
    if (rc != 0) {
      cleanup();
      throw_git_error("git_index_write_tree");
    }

    rc = git_tree_lookup(&tree, repo, &tree_oid);
    if (rc != 0) {
      cleanup();
      throw_git_error("git_tree_lookup");
    }

    rc = git_signature_now(&sig, "Test Helper", "test@helper.local");
    if (rc != 0) {
      cleanup();
      throw_git_error("git_signature_now");
    }

    // Look up parent commit if HEAD exists.
    rc = git_revparse_single(&parent_obj, repo, "HEAD");
    git_oid commit_oid;
    if (rc == 0) {
      rc = git_commit_lookup(&parent_commit, repo, git_object_id(parent_obj));
      if (rc != 0) {
        cleanup();
        throw_git_error("git_commit_lookup(parent)");
      }
      const git_commit *parents[1] = {parent_commit};
      rc = git_commit_create(&commit_oid, repo, "HEAD", sig, sig, nullptr,
                             commit_msg.c_str(), tree, 1, parents);
    } else {
      // No HEAD yet — initial commit.
      git_error_clear();
      rc = git_commit_create_v(&commit_oid, repo, "HEAD", sig, sig, nullptr,
                               commit_msg.c_str(), tree, 0);
    }
    if (rc != 0) {
      cleanup();
      throw_git_error("git_commit_create");
    }

    // Push to origin main.
    rc = git_remote_lookup(&remote, repo, "origin");
    if (rc != 0) {
      cleanup();
      throw_git_error("git_remote_lookup(origin)");
    }

    git_push_options popts = GIT_PUSH_OPTIONS_INIT;
    char refspec_buf[] = "refs/heads/main:refs/heads/main";
    char *refspecs_arr[] = {refspec_buf};
    git_strarray refspecs = {refspecs_arr, 1};
    rc = git_remote_push(remote, &refspecs, &popts);
    if (rc != 0) {
      cleanup();
      throw_git_error("git_remote_push");
    }

    cleanup();
  } catch (...) {
    // cleanup() may have already run; ignore double cleanup errors.
    throw;
  }
}

std::string git_head_sha(const std::string &repo_path) {
  vxcore::LibGit2Init guard;

  git_repository *repo = nullptr;
  check(git_repository_open(&repo, repo_path.c_str()), "git_repository_open");

  git_object *obj = nullptr;
  int rc = git_revparse_single(&obj, repo, "HEAD");
  if (rc != 0) {
    git_repository_free(repo);
    throw_git_error("git_revparse_single(HEAD)");
  }

  char buf[GIT_OID_HEXSZ + 1];
  git_oid_tostr(buf, sizeof(buf), git_object_id(obj));
  std::string sha(buf);

  git_object_free(obj);
  git_repository_free(repo);
  return sha;
}

bool git_status_clean(const std::string &repo_path) {
  vxcore::LibGit2Init guard;

  git_repository *repo = nullptr;
  check(git_repository_open(&repo, repo_path.c_str()), "git_repository_open");

  git_status_options sopts = GIT_STATUS_OPTIONS_INIT;
  git_status_list *list = nullptr;
  int rc = git_status_list_new(&list, repo, &sopts);
  if (rc != 0) {
    git_repository_free(repo);
    throw_git_error("git_status_list_new");
  }
  size_t count = git_status_list_entrycount(list);
  git_status_list_free(list);
  git_repository_free(repo);
  return count == 0;
}

}  // namespace vxcore_test
