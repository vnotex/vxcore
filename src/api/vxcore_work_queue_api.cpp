#include "core/context.h"
#include "core/work_queue.h"
#include "vxcore/vxcore.h"

extern "C" {

VXCORE_API int vxcore_work_queue_process_next(VxCoreContextHandle context, const char *queue_name,
                                              int timeout_ms) {
  if (!context || !queue_name) return 0;
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->work_queue_manager) return 0;
  auto *q = ctx->work_queue_manager->Get(queue_name);
  if (!q) return 0;
  return q->ProcessNext(timeout_ms) ? 1 : 0;
}

VXCORE_API int vxcore_work_queue_process_all(VxCoreContextHandle context, const char *queue_name) {
  if (!context || !queue_name) return 0;
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->work_queue_manager) return 0;
  auto *q = ctx->work_queue_manager->Get(queue_name);
  if (!q) return 0;
  return q->ProcessAll();
}

VXCORE_API int vxcore_work_queue_size(VxCoreContextHandle context, const char *queue_name) {
  if (!context || !queue_name) return 0;
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->work_queue_manager) return 0;
  auto *q = ctx->work_queue_manager->Get(queue_name);
  if (!q) return 0;
  return static_cast<int>(q->Size());
}

VXCORE_API void vxcore_work_queue_shutdown(VxCoreContextHandle context, const char *queue_name) {
  if (!context || !queue_name) return;
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->work_queue_manager) return;
  auto *q = ctx->work_queue_manager->Get(queue_name);
  if (q) q->Shutdown();
}

VXCORE_API void vxcore_work_queue_shutdown_all(VxCoreContextHandle context) {
  if (!context) return;
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->work_queue_manager) return;
  ctx->work_queue_manager->ShutdownAll();
}

}  // extern "C"
