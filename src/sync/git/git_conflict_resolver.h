#ifndef VXCORE_SYNC_GIT_GIT_CONFLICT_RESOLVER_H
#define VXCORE_SYNC_GIT_GIT_CONFLICT_RESOLVER_H

#include <git2.h>

#include <string>
#include <vector>

#include "sync/sync_types.h"  // SyncConflictInfo, SyncConflictResolution

namespace vxcore {

class GitSyncPipeline;  // forward decl — resolver calls pipeline.ContinueRebaseAfterResolution()

class GitConflictResolver {
 public:
  GitConflictResolver(git_repository *repo, const std::string &root_folder);

  VxCoreError GetConflicts(std::vector<SyncConflictInfo> &out_conflicts);
  VxCoreError ResolveConflict(const std::string &path,
                              SyncConflictResolution resolution,
                              GitSyncPipeline &pipeline);

 private:
  git_repository *repo_;
  const std::string &root_folder_;
};

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_CONFLICT_RESOLVER_H
