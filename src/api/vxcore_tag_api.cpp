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
