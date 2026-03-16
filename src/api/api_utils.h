#ifndef VXCORE_API_UTILS_H_
#define VXCORE_API_UTILS_H_

#ifdef _MSC_VER
#define vxcore_strdup _strdup
#else
#define vxcore_strdup strdup
#endif

#include "core/buffer_manager.h"
#include "core/config_manager.h"
#include "core/context.h"
#include "core/workspace_manager.h"

namespace vxcore {

// Persist full session state (buffers + workspaces) to disk.
// Skips if skip_sync_to_session flag is set on the context.
inline void PersistSession(VxCoreContext *ctx) {
  if (!ctx || ctx->skip_sync_to_session) {
    return;
  }
  if (ctx->buffer_manager) {
    ctx->buffer_manager->UpdateSessionBuffers();
  }
  if (ctx->workspace_manager) {
    ctx->workspace_manager->UpdateSessionWorkspaces();
  }
  if (ctx->config_manager) {
    ctx->config_manager->SaveSessionConfig();
  }
}

}  // namespace vxcore

#endif
