#include <stdlib.h>
#include <string.h>

#include <nlohmann/json.hpp>

#include "api/api_utils.h"
#include "core/buffer_manager.h"
#include "core/context.h"
#include "utils/base64.h"
#include "vxcore/vxcore.h"

VXCORE_API VxCoreError vxcore_buffer_open(VxCoreContextHandle context, const char *notebook_id,
                                          const char *file_path, char **out_id) {
  if (!context || !file_path || !out_id) {
    return VXCORE_ERR_NULL_POINTER;
  }
  // notebook_id can be NULL for external files

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->buffer_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    std::string nb_id = notebook_id ? notebook_id : "";
    std::string id = ctx->buffer_manager->OpenBuffer(nb_id, file_path);
    *out_id = vxcore_strdup(id.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_buffer_close(VxCoreContextHandle context, const char *id) {
  if (!context || !id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->buffer_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    bool success = ctx->buffer_manager->CloseBuffer(id);
    if (!success) {
      ctx->last_error = "Buffer not found";
      return VXCORE_ERR_BUFFER_NOT_FOUND;
    }
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_buffer_get(VxCoreContextHandle context, const char *id,
                                         char **out_json) {
  if (!context || !id || !out_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->buffer_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    auto *buffer = ctx->buffer_manager->GetBuffer(id);
    if (!buffer) {
      ctx->last_error = "Buffer not found";
      return VXCORE_ERR_BUFFER_NOT_FOUND;
    }

    // Return JSON without content (content retrieved separately)
    nlohmann::json json = buffer->ToJson();
    json.erase("content");  // Remove large content field
    std::string json_str = json.dump();
    *out_json = vxcore_strdup(json_str.c_str());
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    ctx->last_error = std::string("JSON error: ") + e.what();
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_buffer_list(VxCoreContextHandle context, char **out_json) {
  if (!context || !out_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->buffer_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    auto buffers = ctx->buffer_manager->ListBuffers();
    nlohmann::json json = nlohmann::json::array();
    for (const auto &buf : buffers) {
      nlohmann::json buf_json = buf.ToJson();
      buf_json.erase("content");  // Remove large content field
      json.push_back(buf_json);
    }
    std::string json_str = json.dump();
    *out_json = vxcore_strdup(json_str.c_str());
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    ctx->last_error = std::string("JSON error: ") + e.what();
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_buffer_save(VxCoreContextHandle context, const char *id) {
  if (!context || !id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->buffer_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    VxCoreError err = ctx->buffer_manager->SaveBuffer(id);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to save buffer";
    }
    return err;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_buffer_reload(VxCoreContextHandle context, const char *id) {
  if (!context || !id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->buffer_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    VxCoreError err = ctx->buffer_manager->ReloadBuffer(id);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to reload buffer";
    }
    return err;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_buffer_get_content(VxCoreContextHandle context, const char *id,
                                                 char **out_json) {
  if (!context || !id || !out_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->buffer_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    const void *data;
    size_t size;
    VxCoreError err = ctx->buffer_manager->GetBufferContent(id, &data, &size);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to get buffer content";
      return err;
    }

    // Convert to base64 string for JSON transport
    std::string encoded = vxcore::Base64Encode(static_cast<const uint8_t *>(data), size);

    nlohmann::json json = nlohmann::json::object();
    json["content"] = encoded;
    json["size"] = size;
    std::string json_str = json.dump();
    *out_json = vxcore_strdup(json_str.c_str());
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    ctx->last_error = std::string("JSON error: ") + e.what();
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_buffer_set_content(VxCoreContextHandle context, const char *id,
                                                 const char *content_json) {
  if (!context || !id || !content_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->buffer_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    nlohmann::json json = nlohmann::json::parse(content_json);
    if (!json.contains("content") || !json["content"].is_string()) {
      ctx->last_error = "Invalid content JSON: missing 'content' field";
      return VXCORE_ERR_JSON_PARSE;
    }

    std::string encoded = json["content"].get<std::string>();
    std::vector<uint8_t> data = vxcore::Base64Decode(encoded);
    if (data.empty() && !encoded.empty()) {
      ctx->last_error = "Invalid base64 content";
      return VXCORE_ERR_JSON_PARSE;
    }

    VxCoreError err = ctx->buffer_manager->SetBufferContent(id, data.data(), data.size());
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to set buffer content";
    }
    return err;
  } catch (const nlohmann::json::exception &e) {
    ctx->last_error = std::string("JSON error: ") + e.what();
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_buffer_get_content_raw(VxCoreContextHandle context, const char *id,
                                                     const void **out_data, size_t *out_size) {
  if (!context || !id || !out_data || !out_size) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->buffer_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    VxCoreError err = ctx->buffer_manager->GetBufferContent(id, out_data, out_size);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to get buffer content";
    }
    return err;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_buffer_set_content_raw(VxCoreContextHandle context, const char *id,
                                                     const void *data, size_t size) {
  if (!context || !id || !data) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->buffer_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    VxCoreError err = ctx->buffer_manager->SetBufferContent(id, data, size);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to set buffer content";
    }
    return err;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_buffer_get_state(VxCoreContextHandle context, const char *id,
                                               VxCoreBufferState *out_state) {
  if (!context || !id || !out_state) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->buffer_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    auto *buffer = ctx->buffer_manager->GetBuffer(id);
    if (!buffer) {
      ctx->last_error = "Buffer not found";
      return VXCORE_ERR_BUFFER_NOT_FOUND;
    }

    *out_state = buffer->state;
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_buffer_is_modified(VxCoreContextHandle context, const char *id,
                                                 int *out_modified) {
  if (!context || !id || !out_modified) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->buffer_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    auto *buffer = ctx->buffer_manager->GetBuffer(id);
    if (!buffer) {
      ctx->last_error = "Buffer not found";
      return VXCORE_ERR_BUFFER_NOT_FOUND;
    }

    *out_modified = buffer->modified ? 1 : 0;
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_buffer_auto_save_tick(VxCoreContextHandle context) {
  if (!context) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->buffer_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    ctx->buffer_manager->AutoSaveTick();
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_buffer_set_auto_save_interval(VxCoreContextHandle context,
                                                            int64_t interval_ms) {
  if (!context) {
    return VXCORE_ERR_NULL_POINTER;
  }
  if (interval_ms < 0) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->buffer_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    ctx->buffer_manager->SetAutoSaveInterval(interval_ms);
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}
