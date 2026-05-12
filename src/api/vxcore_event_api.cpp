#include <string>
#include <unordered_map>
#include <vector>

#include "core/context.h"
#include "core/event_manager.h"
#include "vxcore/vxcore.h"

namespace {

struct CCallbackEntry {
  VxCoreEventCallback c_callback;
  void *userdata;
  vxcore::EventManager::ListenerId listener_id;
};

std::unordered_map<VxCoreContextHandle, std::vector<CCallbackEntry>> g_c_callbacks;
std::mutex g_c_callbacks_mutex;

}  // namespace

extern "C" {

VXCORE_API VxCoreError vxcore_on_event(VxCoreContextHandle context, const char *event_name,
                                       VxCoreEventCallback callback, void *userdata) {
  if (!context || !event_name || !callback) return VXCORE_ERR_INVALID_PARAM;
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->event_manager) return VXCORE_ERR_INVALID_STATE;

  auto c_cb = callback;
  auto c_ud = userdata;
  auto id = ctx->event_manager->Subscribe(
      event_name, [c_cb, c_ud](const std::string &name, const nlohmann::json &data) {
        std::string json_str = data.dump();
        c_cb(name.c_str(), json_str.c_str(), c_ud);
      });

  {
    std::lock_guard<std::mutex> lock(g_c_callbacks_mutex);
    g_c_callbacks[context].push_back({callback, userdata, id});
  }
  return VXCORE_OK;
}

VXCORE_API VxCoreError vxcore_off_event(VxCoreContextHandle context, const char *event_name,
                                        VxCoreEventCallback callback) {
  if (!context || !event_name || !callback) return VXCORE_ERR_INVALID_PARAM;
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->event_manager) return VXCORE_ERR_INVALID_STATE;

  std::lock_guard<std::mutex> lock(g_c_callbacks_mutex);
  auto it = g_c_callbacks.find(context);
  if (it == g_c_callbacks.end()) return VXCORE_ERR_NOT_FOUND;

  auto &entries = it->second;
  for (auto entry_it = entries.begin(); entry_it != entries.end(); ++entry_it) {
    if (entry_it->c_callback == callback) {
      ctx->event_manager->Unsubscribe(entry_it->listener_id);
      entries.erase(entry_it);
      return VXCORE_OK;
    }
  }
  return VXCORE_ERR_NOT_FOUND;
}

}  // extern "C"
