#ifndef VXCORE_SEARCH_QUEUE_NAME_H
#define VXCORE_SEARCH_QUEUE_NAME_H

namespace vxcore {

// Single source of truth for the dedicated content-search work queue name.
// Used by the api layer to pre-create (vxcore_context_create) and resolve
// (vxcore_search_content / vxcore_search_content_ex) the same named queue.
constexpr char kSearchQueueName[] = "vxcore.search";

}  // namespace vxcore

#endif  // VXCORE_SEARCH_QUEUE_NAME_H
