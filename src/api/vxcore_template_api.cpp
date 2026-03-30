#include <stdlib.h>

#include <nlohmann/json.hpp>

#include "api/api_utils.h"
#include "core/context.h"
#include "core/template_manager.h"
#include "vxcore/vxcore.h"

VXCORE_API VxCoreError vxcore_template_get_folder_path(VxCoreContextHandle context,
                                                       char **out_path) {
  if (!context || !out_path) {
    return VXCORE_ERR_NULL_POINTER;
  }

  *out_path = nullptr;

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->template_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    std::string path;
    VxCoreError err = ctx->template_manager->GetTemplateFolderPath(path);
    if (err != VXCORE_OK) return err;
    *out_path = vxcore_strdup(path.c_str());
    if (!*out_path) return VXCORE_ERR_OUT_OF_MEMORY;
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_template_list(VxCoreContextHandle context, char **out_json) {
  if (!context || !out_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  *out_json = nullptr;

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->template_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    std::vector<std::string> names;
    VxCoreError err = ctx->template_manager->ListTemplates(names);
    if (err != VXCORE_OK) return err;

    nlohmann::json json_array = nlohmann::json::array();
    for (const auto &name : names) {
      json_array.push_back(name);
    }

    std::string json_str = json_array.dump();
    *out_json = vxcore_strdup(json_str.c_str());
    if (!*out_json) return VXCORE_ERR_OUT_OF_MEMORY;
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    ctx->last_error = std::string("JSON error: ") + e.what();
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_template_list_by_suffix(VxCoreContextHandle context,
                                                      const char *suffix, char **out_json) {
  if (!context || !suffix || !out_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  *out_json = nullptr;

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->template_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    std::vector<std::string> names;
    VxCoreError err = ctx->template_manager->ListTemplatesBySuffix(suffix, names);
    if (err != VXCORE_OK) return err;

    nlohmann::json json_array = nlohmann::json::array();
    for (const auto &name : names) {
      json_array.push_back(name);
    }

    std::string json_str = json_array.dump();
    *out_json = vxcore_strdup(json_str.c_str());
    if (!*out_json) return VXCORE_ERR_OUT_OF_MEMORY;
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &e) {
    ctx->last_error = std::string("JSON error: ") + e.what();
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_template_get_content(VxCoreContextHandle context, const char *name,
                                                   char **out_content) {
  if (!context || !name || !out_content) {
    return VXCORE_ERR_NULL_POINTER;
  }

  *out_content = nullptr;

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->template_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    std::string content;
    VxCoreError err = ctx->template_manager->GetTemplateContent(name, content);
    if (err != VXCORE_OK) return err;
    *out_content = vxcore_strdup(content.c_str());
    if (!*out_content) return VXCORE_ERR_OUT_OF_MEMORY;
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_template_create(VxCoreContextHandle context, const char *name,
                                              const char *content) {
  if (!context || !name || !content) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->template_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    return ctx->template_manager->CreateTemplate(name, content);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_template_delete(VxCoreContextHandle context, const char *name) {
  if (!context || !name) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->template_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    return ctx->template_manager->DeleteTemplate(name);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_template_rename(VxCoreContextHandle context, const char *old_name,
                                              const char *new_name) {
  if (!context || !old_name || !new_name) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->template_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    return ctx->template_manager->RenameTemplate(old_name, new_name);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}
