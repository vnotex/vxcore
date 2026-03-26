#include <nlohmann/json.hpp>

#include "api/api_utils.h"
#include "core/context.h"
#include "core/notebook_manager.h"
#include "vxcore/vxcore.h"

VXCORE_API VxCoreError vxcore_tag_create(VxCoreContextHandle context, const char *notebook_id,
                                         const char *tag_name) {
  if (!context || !notebook_id || !tag_name) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    VxCoreError err = notebook->CreateTag(tag_name, "");
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to create tag";
      return err;
    }

    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error creating tag";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_create_path(VxCoreContextHandle context, const char *notebook_id,
                                              const char *tag_path) {
  if (!context || !notebook_id || !tag_path) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    VxCoreError err = notebook->CreateTagPath(tag_path);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to create tag path";
      return err;
    }

    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error creating tag path";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_delete(VxCoreContextHandle context, const char *notebook_id,
                                         const char *tag_name) {
  if (!context || !notebook_id || !tag_name) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    VxCoreError err = notebook->DeleteTag(tag_name);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to delete tag";
      return err;
    }

    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error deleting tag";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_list(VxCoreContextHandle context, const char *notebook_id,
                                       char **out_tags_json) {
  if (!context || !notebook_id || !out_tags_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    std::string tags_json;
    VxCoreError err = notebook->GetTags(tags_json);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to get tags";
      return err;
    }

    char *json_copy = vxcore_strdup(tags_json.c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_tags_json = json_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error listing tags";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_move(VxCoreContextHandle context, const char *notebook_id,
                                       const char *tag_name, const char *parent_tag) {
  if (!context || !notebook_id || !tag_name || !parent_tag) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    VxCoreError err = notebook->MoveTag(tag_name, parent_tag);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to move tag";
      return err;
    }

    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error moving tag";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_find_files(VxCoreContextHandle context, const char *notebook_id,
                                             const char *tags_json, const char *op,
                                             char **out_results_json) {
  if (!context || !notebook_id || !tags_json || !op || !out_results_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    // Parse tags array
    nlohmann::json tags_array;
    try {
      tags_array = nlohmann::json::parse(tags_json);
    } catch (...) {
      ctx->last_error = "Invalid tags JSON";
      return VXCORE_ERR_JSON_PARSE;
    }

    if (!tags_array.is_array()) {
      ctx->last_error = "tags_json must be a JSON array";
      return VXCORE_ERR_INVALID_PARAM;
    }

    std::vector<std::string> tags;
    for (const auto &tag : tags_array) {
      if (!tag.is_string()) {
        ctx->last_error = "Each tag must be a string";
        return VXCORE_ERR_INVALID_PARAM;
      }
      tags.push_back(tag.get<std::string>());
    }

    // Parse operator
    std::string op_str(op);
    bool use_and;
    if (op_str == "AND") {
      use_and = true;
    } else if (op_str == "OR") {
      use_and = false;
    } else {
      ctx->last_error = "Operator must be \"AND\" or \"OR\"";
      return VXCORE_ERR_INVALID_PARAM;
    }

    std::string results_json;
    VxCoreError err = notebook->FindFilesByTags(tags, use_and, results_json);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to find files by tags";
      return err;
    }

    char *json_copy = vxcore_strdup(results_json.c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_results_json = json_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error finding files by tags";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_count_files_by_tag(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     char **out_results_json) {
  if (!context || !notebook_id || !out_results_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    std::string results_json;
    VxCoreError err = notebook->CountFilesByTag(results_json);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to count files by tag";
      return err;
    }

    char *json_copy = vxcore_strdup(results_json.c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_results_json = json_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error counting files by tag";
    return VXCORE_ERR_UNKNOWN;
  }
}
