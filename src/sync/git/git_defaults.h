#ifndef VXCORE_SYNC_GIT_GIT_DEFAULTS_H
#define VXCORE_SYNC_GIT_GIT_DEFAULTS_H

#include <string>

#include "sync/sync_types.h"  // For SyncConfig

namespace vxcore {

// Default .gitignore for VNote sync repos. Excludes the libgit2 gitdir
// (vx_notebook/vx_sync/) and common transient/OS files. User edits to the
// on-disk .gitignore are preserved (WriteIfMissing skips existing files).
extern const char *const kDefaultGitignore;

// Default .gitattributes: normalize text line endings, mark common binary types
// so libgit2 / git skips text merges on them.
extern const char *const kDefaultGitattributes;

// Default commit author name when credentials_.author_name is empty.
extern const char *const kDefaultAuthorName;

// Default commit author email when credentials_.author_email is empty.
extern const char *const kDefaultAuthorEmail;

// Write |content| to |abs_path| only if the file does not already exist.
// Returns true if the file was created, false if it already existed or on error.
// UTF-8 safe on Windows: uses PathExists / PathFromUtf8.
bool WriteIfMissing(const std::string &abs_path, const std::string &content);

// Build the full .gitignore content for a given config: defaults plus any
// user-configured extra excludes from SyncConfig::exclude_paths.
std::string BuildGitignoreContent(const SyncConfig &config);

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_DEFAULTS_H
