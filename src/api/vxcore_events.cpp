#include "vxcore/vxcore_events.h"
#include "vxcore/vxcore.h"

VXCORE_API VxCoreError vxcore_event_subscribe(VxCoreContextHandle context,
                                              VxCoreEventType event_type,
                                              VxCoreEventCallback callback, void *user_data) {
  (void)context;
  (void)event_type;
  (void)callback;
  (void)user_data;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_event_unsubscribe(VxCoreContextHandle context,
                                                VxCoreEventType event_type,
                                                VxCoreEventCallback callback) {
  (void)context;
  (void)event_type;
  (void)callback;
  return VXCORE_ERR_UNSUPPORTED;
}
