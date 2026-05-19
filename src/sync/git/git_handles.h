#ifndef VXCORE_SYNC_GIT_GIT_HANDLES_H
#define VXCORE_SYNC_GIT_GIT_HANDLES_H

#include <git2.h>

#include <memory>

namespace vxcore {

// RAII unique_ptr aliases for libgit2 handle types.
//
// Used in NEW extracted code only (T5-T12). Existing cleanup blocks in
// git_sync_backend.cpp are NOT being converted in this restructure plan —
// that's a separate audit-Phase-B follow-up.
//
// Pattern: each libgit2 handle type gets a deleter struct calling the
// matching git_*_free() function, and a unique_ptr alias for ergonomics.

#define VXCORE_GIT_DELETER(Type, FreeFn)                          \
  struct Type##Deleter {                                          \
    void operator()(::Type *p) const noexcept {                   \
      if (p) {                                                    \
        FreeFn(p);                                                \
      }                                                           \
    }                                                             \
  };                                                              \
  using Type##Ptr = std::unique_ptr<::Type, Type##Deleter>

VXCORE_GIT_DELETER(git_repository, git_repository_free);
VXCORE_GIT_DELETER(git_remote, git_remote_free);
VXCORE_GIT_DELETER(git_index, git_index_free);
VXCORE_GIT_DELETER(git_tree, git_tree_free);
VXCORE_GIT_DELETER(git_commit, git_commit_free);
VXCORE_GIT_DELETER(git_reference, git_reference_free);
VXCORE_GIT_DELETER(git_signature, git_signature_free);
VXCORE_GIT_DELETER(git_status_list, git_status_list_free);
VXCORE_GIT_DELETER(git_blob, git_blob_free);
VXCORE_GIT_DELETER(git_object, git_object_free);
VXCORE_GIT_DELETER(git_annotated_commit, git_annotated_commit_free);
VXCORE_GIT_DELETER(git_rebase, git_rebase_free);
VXCORE_GIT_DELETER(git_config, git_config_free);
VXCORE_GIT_DELETER(git_index_conflict_iterator, git_index_conflict_iterator_free);

#undef VXCORE_GIT_DELETER

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_HANDLES_H
