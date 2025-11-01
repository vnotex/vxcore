#ifndef VXCORE_EVENTS_H
#define VXCORE_EVENTS_H

#include "vxcore_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  VXCORE_EVENT_NOTE_CREATED = 1,
  VXCORE_EVENT_NOTE_UPDATED = 2,
  VXCORE_EVENT_NOTE_DELETED = 3,
  VXCORE_EVENT_NOTE_MOVED = 4,
  VXCORE_EVENT_TAG_ADDED = 5,
  VXCORE_EVENT_TAG_REMOVED = 6,
  VXCORE_EVENT_ATTACHMENT_ADDED = 7,
  VXCORE_EVENT_ATTACHMENT_REMOVED = 8,
  VXCORE_EVENT_NOTEBOOK_OPENED = 9,
  VXCORE_EVENT_NOTEBOOK_CLOSED = 10,
  VXCORE_EVENT_INDEX_UPDATED = 11
} VxCoreEventType;

typedef struct {
  VxCoreEventType type;
  const char *payload_json;
  uint64_t timestamp_ms;
} VxCoreEvent;

typedef void (*VxCoreEventCallback)(const VxCoreEvent *event, void *user_data);

VXCORE_API VxCoreError vxcore_event_subscribe(VxCoreContextHandle context,
                                              VxCoreEventType event_type,
                                              VxCoreEventCallback callback, void *user_data);

VXCORE_API VxCoreError vxcore_event_unsubscribe(VxCoreContextHandle context,
                                                VxCoreEventType event_type,
                                                VxCoreEventCallback callback);

#ifdef __cplusplus
}
#endif

#endif
