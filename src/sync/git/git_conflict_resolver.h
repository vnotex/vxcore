#ifndef VXCORE_SYNC_GIT_GIT_CONFLICT_RESOLVER_H
#define VXCORE_SYNC_GIT_GIT_CONFLICT_RESOLVER_H

#include <git2.h>

#include <cstdint>
#include <string>
#include <vector>

#include "sync/sync_types.h"  // SyncConflictInfo, SyncConflictResolution

namespace vxcore {

class GitSyncPipeline;  // forward decl — resolver calls pipeline.ContinueRebaseAfterResolution()

// Tri-state (four-state) outcome of a conflict resolution attempt.
// Replaces the previous bool/VxCoreError collapse which lost information —
// callers can now distinguish "applied a resolution" from "nothing was in
// conflict" from "user aborted" from "unrecoverable error". The C ABI layer
// (vxcore_sync_resolve_conflict) still returns VxCoreError; the bridge lives
// in GitSyncBackend::ResolveConflict which maps this enum back to a single
// error code. Wave 7 of sync-backend-phase4 will redo the C ABI to surface
// the richer state directly.
enum class ResolveResult : uint8_t {
  Resolved,    // A conflict existed and was successfully resolved (rebase resumed).
  NoConflict,  // No conflict existed at the given path — trivially nothing to do.
  Aborted,     // Caller-driven abort; working tree was cleaned to a known state.
  Failed,      // Unrecoverable error (libgit2 failure, IO failure, etc.).
};

class GitConflictResolver {
 public:
  GitConflictResolver(git_repository *repo, const std::string &root_folder);

  VxCoreError GetConflicts(std::vector<SyncConflictInfo> &out_conflicts);

  // Tri-state resolver entry point. Prefer this in new C++ code so callers
  // can react differently to "Resolved" vs "NoConflict" vs "Aborted" vs
  // "Failed". The legacy VxCoreError-returning shim below preserves the
  // existing C ABI contract.
  ResolveResult ResolveConflictEx(const std::string &path,
                                  SyncConflictResolution resolution,
                                  GitSyncPipeline &pipeline);

  // Legacy entry point retained for the C ABI bridge in GitSyncBackend.
  // Internally delegates to ResolveConflictEx and maps the enum back to a
  // VxCoreError using MapResolveResult().
  VxCoreError ResolveConflict(const std::string &path,
                              SyncConflictResolution resolution,
                              GitSyncPipeline &pipeline);

 private:
  git_repository *repo_;
  const std::string &root_folder_;
};

// Maps a ResolveResult onto a VxCoreError using the legacy collapsing rule
// (Resolved + NoConflict → VXCORE_OK; Aborted + Failed → error code). Lives
// in the header so the C ABI bridge in vxcore_sync_api.cpp / GitSyncBackend
// can reuse it once Wave 7 expands the C ABI.
inline VxCoreError MapResolveResult(ResolveResult r) {
  switch (r) {
    case ResolveResult::Resolved:
      return VXCORE_OK;
    case ResolveResult::NoConflict:
      return VXCORE_OK;
    case ResolveResult::Aborted:
      return VXCORE_ERR_UNKNOWN;
    case ResolveResult::Failed:
      return VXCORE_ERR_UNKNOWN;
  }
  return VXCORE_ERR_UNKNOWN;
}

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_CONFLICT_RESOLVER_H
