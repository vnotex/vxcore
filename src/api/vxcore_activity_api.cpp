#include <nlohmann/json.hpp>

#include "api/api_utils.h"
#include "core/activity_manager.h"
#include "core/context.h"
#include "vxcore/vxcore.h"

namespace {

// Emit a JSON string result through an out param, mirroring the tag/sync API
// shape (strdup + OUT_OF_MEMORY guard).
VxCoreError EmitJson(const std::string &json, char **out_json) {
  char *copy = vxcore_strdup(json.c_str());
  if (!copy) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  }
  *out_json = copy;
  return VXCORE_OK;
}

}  // namespace

VXCORE_API VxCoreError vxcore_activity_add_focus_time(VxCoreContextHandle context,
                                                      int64_t delta_ms) {
  if (!context) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    if (!ctx->activity_manager) {
      return VXCORE_ERR_NOT_INITIALIZED;
    }
    return ctx->activity_manager->AddFocusTime(delta_ms);
  } catch (...) {
    ctx->last_error = "Unknown error adding focus time";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_activity_record_read(VxCoreContextHandle context,
                                                   const char *notebook_id, const char *rel_path) {
  if (!context || !notebook_id || !rel_path) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    if (!ctx->activity_manager) {
      return VXCORE_ERR_NOT_INITIALIZED;
    }
    return ctx->activity_manager->RecordRead(notebook_id, rel_path);
  } catch (...) {
    ctx->last_error = "Unknown error recording read";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_activity_record_edit(VxCoreContextHandle context,
                                                   const char *notebook_id, const char *rel_path) {
  if (!context || !notebook_id || !rel_path) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    if (!ctx->activity_manager) {
      return VXCORE_ERR_NOT_INITIALIZED;
    }
    return ctx->activity_manager->RecordEdit(notebook_id, rel_path);
  } catch (...) {
    ctx->last_error = "Unknown error recording edit";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_activity_flush(VxCoreContextHandle context) {
  if (!context) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    if (!ctx->activity_manager) {
      return VXCORE_ERR_NOT_INITIALIZED;
    }
    return ctx->activity_manager->Flush();
  } catch (...) {
    ctx->last_error = "Unknown error flushing activity";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_activity_get_range(VxCoreContextHandle context, const char *from_date,                                                 const char *to_date, char **out_json) {
  if (!context || !from_date || !to_date || !out_json) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    if (!ctx->activity_manager) {
      return VXCORE_ERR_NOT_INITIALIZED;
    }
    std::string json;
    VxCoreError err = ctx->activity_manager->GetRange(from_date, to_date, json);
    if (err != VXCORE_OK) {
      return err;
    }
    return EmitJson(json, out_json);
  } catch (...) {
    ctx->last_error = "Unknown error querying activity range";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_activity_get_hot_files(VxCoreContextHandle context,
                                                     const char *from_date, const char *to_date,
                                                     int limit, char **out_json) {
  if (!context || !from_date || !to_date || !out_json) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    if (!ctx->activity_manager) {
      return VXCORE_ERR_NOT_INITIALIZED;
    }
    std::string json;
    VxCoreError err = ctx->activity_manager->GetHotFiles(from_date, to_date, limit, json);
    if (err != VXCORE_OK) {
      return err;
    }
    return EmitJson(json, out_json);
  } catch (...) {
    ctx->last_error = "Unknown error querying hot files";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_activity_get_file_history(VxCoreContextHandle context,
                                                        const char *notebook_id,
                                                        const char *file_id, char **out_json) {
  if (!context || !notebook_id || !file_id || !out_json) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    if (!ctx->activity_manager) {
      return VXCORE_ERR_NOT_INITIALIZED;
    }
    std::string json;
    VxCoreError err = ctx->activity_manager->GetFileHistory(notebook_id, file_id, json);
    if (err != VXCORE_OK) {
      return err;
    }
    return EmitJson(json, out_json);
  } catch (...) {
    ctx->last_error = "Unknown error querying file history";
    return VXCORE_ERR_UNKNOWN;
  }
}
