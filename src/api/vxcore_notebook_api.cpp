#include <stdlib.h>
#include <string.h>

#include "api/api_utils.h"
#include "core/context.h"
#include "core/notebook_manager.h"
#include "vxcore/vxcore.h"

VXCORE_API VxCoreError vxcore_notebook_create(VxCoreContextHandle context, const char *path,
                                              const char *properties_json, VxCoreNotebookType type,
                                              char **out_notebook_id) {
  if (!context || !path || !out_notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::NotebookType notebook_type =
        (type == VXCORE_NOTEBOOK_RAW) ? vxcore::NotebookType::Raw : vxcore::NotebookType::Bundled;

    std::string properties_str = properties_json ? properties_json : "";
    std::string notebook_id;

    VxCoreError err =
        ctx->notebook_manager->CreateNotebook(path, notebook_type, properties_str, notebook_id);

    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to create notebook";
      return err;
    }

    char *id_copy = vxcore_strdup(notebook_id.c_str());
    if (!id_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_notebook_id = id_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error creating notebook";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_open(VxCoreContextHandle context, const char *path,
                                            char **out_notebook_id) {
  if (!context || !path || !out_notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    std::string notebook_id;
    VxCoreError err = ctx->notebook_manager->OpenNotebook(path, notebook_id);

    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to open notebook";
      return err;
    }

    char *id_copy = vxcore_strdup(notebook_id.c_str());
    if (!id_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_notebook_id = id_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error opening notebook";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_close(VxCoreContextHandle context, const char *notebook_id) {
  if (!context || !notebook_id) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    VxCoreError err = ctx->notebook_manager->CloseNotebook(notebook_id);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to close notebook";
    }
    return err;
  } catch (...) {
    ctx->last_error = "Unknown error closing notebook";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_list(VxCoreContextHandle context,
                                            char **out_notebooks_json) {
  if (!context || !out_notebooks_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    std::string notebooks_json;
    VxCoreError err = ctx->notebook_manager->ListNotebooks(notebooks_json);

    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to list notebooks";
      return err;
    }

    char *json_copy = vxcore_strdup(notebooks_json.c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_notebooks_json = json_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error listing notebooks";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_get_properties(VxCoreContextHandle context,
                                                      const char *notebook_id,
                                                      char **out_properties_json) {
  if (!context || !notebook_id || !out_properties_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    std::string properties_json;
    VxCoreError err = ctx->notebook_manager->GetNotebookProperties(notebook_id, properties_json);

    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to get notebook properties";
      return err;
    }

    char *json_copy = vxcore_strdup(properties_json.c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_properties_json = json_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error getting notebook properties";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_notebook_set_properties(VxCoreContextHandle context,
                                                      const char *notebook_id,
                                                      const char *properties_json) {
  if (!context || !notebook_id || !properties_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    VxCoreError err = ctx->notebook_manager->SetNotebookProperties(notebook_id, properties_json);

    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to set notebook properties";
    }
    return err;
  } catch (...) {
    ctx->last_error = "Unknown error setting notebook properties";
    return VXCORE_ERR_UNKNOWN;
  }
}
