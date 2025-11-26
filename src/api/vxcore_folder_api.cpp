#include "api/api_utils.h"
#include "core/context.h"
#include "core/folder_manager.h"
#include "core/notebook_manager.h"
#include "vxcore/vxcore.h"

VXCORE_API VxCoreError vxcore_folder_create(VxCoreContextHandle context, const char *notebook_id,
                                            const char *parent_path, const char *folder_name,
                                            char **out_folder_id) {
  if (!context || !notebook_id || !folder_name || !out_folder_id) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    if (notebook->GetType() != vxcore::NotebookType::Bundled) {
      ctx->last_error = "Operation not supported for raw notebooks";
      return VXCORE_ERR_UNSUPPORTED;
    }

    vxcore::FolderManager folder_manager(notebook);
    std::string folder_id;
    std::string parent = parent_path ? parent_path : ".";

    VxCoreError error = folder_manager.CreateFolder(parent, folder_name, folder_id);
    if (error != VXCORE_OK) {
      return error;
    }

    *out_folder_id = vxcore_strdup(folder_id.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_folder_delete(VxCoreContextHandle context, const char *notebook_id,
                                            const char *folder_path) {
  if (!context || !notebook_id || !folder_path) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    if (notebook->GetType() != vxcore::NotebookType::Bundled) {
      ctx->last_error = "Operation not supported for raw notebooks";
      return VXCORE_ERR_UNSUPPORTED;
    }

    vxcore::FolderManager folder_manager(notebook);
    return folder_manager.DeleteFolder(folder_path);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_folder_get_config(VxCoreContextHandle context,
                                                const char *notebook_id, const char *folder_path,
                                                char **out_config_json) {
  if (!context || !notebook_id || !folder_path || !out_config_json) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    if (notebook->GetType() != vxcore::NotebookType::Bundled) {
      ctx->last_error = "Operation not supported for raw notebooks";
      return VXCORE_ERR_UNSUPPORTED;
    }

    vxcore::FolderManager folder_manager(notebook);
    std::string config_json;

    VxCoreError error = folder_manager.GetFolderConfig(folder_path, config_json);
    if (error != VXCORE_OK) {
      return error;
    }

    *out_config_json = vxcore_strdup(config_json.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_folder_update_metadata(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     const char *folder_path,
                                                     const char *metadata_json) {
  if (!context || !notebook_id || !folder_path || !metadata_json) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    if (notebook->GetType() != vxcore::NotebookType::Bundled) {
      ctx->last_error = "Operation not supported for raw notebooks";
      return VXCORE_ERR_UNSUPPORTED;
    }

    vxcore::FolderManager folder_manager(notebook);
    return folder_manager.UpdateFolderMetadata(folder_path, metadata_json);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_file_create(VxCoreContextHandle context, const char *notebook_id,
                                          const char *folder_path, const char *file_name,
                                          char **out_file_id) {
  if (!context || !notebook_id || !folder_path || !file_name || !out_file_id) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    if (notebook->GetType() != vxcore::NotebookType::Bundled) {
      ctx->last_error = "Operation not supported for raw notebooks";
      return VXCORE_ERR_UNSUPPORTED;
    }

    vxcore::FolderManager folder_manager(notebook);
    std::string file_id;

    VxCoreError error = folder_manager.CreateFile(folder_path, file_name, file_id);
    if (error != VXCORE_OK) {
      return error;
    }

    *out_file_id = vxcore_strdup(file_id.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_file_delete(VxCoreContextHandle context, const char *notebook_id,
                                          const char *folder_path, const char *file_name) {
  if (!context || !notebook_id || !folder_path || !file_name) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    if (notebook->GetType() != vxcore::NotebookType::Bundled) {
      ctx->last_error = "Operation not supported for raw notebooks";
      return VXCORE_ERR_UNSUPPORTED;
    }

    vxcore::FolderManager folder_manager(notebook);
    return folder_manager.DeleteFile(folder_path, file_name);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_file_update_metadata(VxCoreContextHandle context,
                                                   const char *notebook_id, const char *folder_path,
                                                   const char *file_name,
                                                   const char *metadata_json) {
  if (!context || !notebook_id || !folder_path || !file_name || !metadata_json) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    if (notebook->GetType() != vxcore::NotebookType::Bundled) {
      ctx->last_error = "Operation not supported for raw notebooks";
      return VXCORE_ERR_UNSUPPORTED;
    }

    vxcore::FolderManager folder_manager(notebook);
    return folder_manager.UpdateFileMetadata(folder_path, file_name, metadata_json);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_file_update_tags(VxCoreContextHandle context, const char *notebook_id,
                                               const char *folder_path, const char *file_name,
                                               const char *tags_json) {
  if (!context || !notebook_id || !folder_path || !file_name || !tags_json) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    if (notebook->GetType() != vxcore::NotebookType::Bundled) {
      ctx->last_error = "Operation not supported for raw notebooks";
      return VXCORE_ERR_UNSUPPORTED;
    }

    vxcore::FolderManager folder_manager(notebook);
    return folder_manager.UpdateFileTags(folder_path, file_name, tags_json);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}
