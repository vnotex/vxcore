#ifndef VXCORE_TEST_GIT_SYNC_HELPERS_H
#define VXCORE_TEST_GIT_SYNC_HELPERS_H

#include <string>

namespace vxcore_test {

// Initialize a NEW bare repository at <bare_path> with initial_head="main".
// Creates parent directories as needed. If <bare_path> already exists, it is
// removed first (so tests can be re-run).
// Returns the file:// URL for the bare repo (e.g. "file:///C:/temp/foo.git").
std::string create_bare_repo(const std::string &bare_path);

// Clone the bare repo to a temporary working dir, write <rel_file> with <content>,
// git add + commit (author "Test Helper" <test@helper.local>), push back to bare,
// remove the working clone. <commit_msg> goes in the commit log.
// Throws std::runtime_error on any libgit2 failure (with git_error_last() text).
void commit_file(const std::string &bare_repo_path, const std::string &rel_file,
                 const std::string &content, const std::string &commit_msg);

// Open the repo at <repo_path> (bare or non-bare), revparse "HEAD",
// return the OID hex string (40 chars). Throws on failure.
std::string git_head_sha(const std::string &repo_path);

// Return true iff git_status_list_new on <repo_path> is empty.
// <repo_path> must be a non-bare working tree. Throws on failure.
bool git_status_clean(const std::string &repo_path);

}  // namespace vxcore_test

#endif  // VXCORE_TEST_GIT_SYNC_HELPERS_H
