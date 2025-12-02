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

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }

    std::string folder_id;
    std::string parent = parent_path ? parent_path : ".";

    VxCoreError error = folder_manager->CreateFolder(parent, folder_name, folder_id);
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

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    return folder_manager->DeleteFolder(folder_path);
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

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    std::string config_json;

    VxCoreError error = folder_manager->GetFolderConfig(folder_path, config_json);
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

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    return folder_manager->UpdateFolderMetadata(folder_path, metadata_json);
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

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    std::string file_id;

    VxCoreError error = folder_manager->CreateFile(folder_path, file_name, file_id);
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

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    return folder_manager->DeleteFile(folder_path, file_name);
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

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    return folder_manager->UpdateFileMetadata(folder_path, file_name, metadata_json);
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

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    return folder_manager->UpdateFileTags(folder_path, file_name, tags_json);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_folder_get_metadata(VxCoreContextHandle context,
                                                  const char *notebook_id, const char *folder_path,
                                                  char **out_metadata_json) {
  if (!context || !notebook_id || !folder_path || !out_metadata_json) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    std::string metadata_json;

    VxCoreError error = folder_manager->GetFolderMetadata(folder_path, metadata_json);
    if (error != VXCORE_OK) {
      return error;
    }

    *out_metadata_json = vxcore_strdup(metadata_json.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_folder_rename(VxCoreContextHandle context, const char *notebook_id,
                                            const char *folder_path, const char *new_name) {
  if (!context || !notebook_id || !folder_path || !new_name) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    return folder_manager->RenameFolder(folder_path, new_name);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_folder_move(VxCoreContextHandle context, const char *notebook_id,
                                          const char *src_path, const char *dest_parent_path) {
  if (!context || !notebook_id || !src_path || !dest_parent_path) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    return folder_manager->MoveFolder(src_path, dest_parent_path);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_folder_copy(VxCoreContextHandle context, const char *notebook_id,
                                          const char *src_path, const char *dest_parent_path,
                                          const char *new_name, char **out_folder_id) {
  if (!context || !notebook_id || !src_path || !dest_parent_path || !out_folder_id) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    std::string target_name = new_name ? new_name : "";
    std::string folder_id;

    VxCoreError error =
        folder_manager->CopyFolder(src_path, dest_parent_path, target_name, folder_id);
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

VXCORE_API VxCoreError vxcore_file_get_info(VxCoreContextHandle context, const char *notebook_id,
                                            const char *folder_path, const char *file_name,
                                            char **out_file_info_json) {
  if (!context || !notebook_id || !folder_path || !file_name || !out_file_info_json) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    std::string file_info_json;

    VxCoreError error = folder_manager->GetFileInfo(folder_path, file_name, file_info_json);
    if (error != VXCORE_OK) {
      return error;
    }

    *out_file_info_json = vxcore_strdup(file_info_json.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_file_get_metadata(VxCoreContextHandle context,
                                                const char *notebook_id, const char *folder_path,
                                                const char *file_name, char **out_metadata_json) {
  if (!context || !notebook_id || !folder_path || !file_name || !out_metadata_json) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    std::string metadata_json;

    VxCoreError error = folder_manager->GetFileMetadata(folder_path, file_name, metadata_json);
    if (error != VXCORE_OK) {
      return error;
    }

    *out_metadata_json = vxcore_strdup(metadata_json.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_file_rename(VxCoreContextHandle context, const char *notebook_id,
                                          const char *folder_path, const char *old_name,
                                          const char *new_name) {
  if (!context || !notebook_id || !folder_path || !old_name || !new_name) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    return folder_manager->RenameFile(folder_path, old_name, new_name);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_file_move(VxCoreContextHandle context, const char *notebook_id,
                                        const char *src_folder_path, const char *file_name,
                                        const char *dest_folder_path) {
  if (!context || !notebook_id || !src_folder_path || !file_name || !dest_folder_path) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    return folder_manager->MoveFile(src_folder_path, file_name, dest_folder_path);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_file_copy(VxCoreContextHandle context, const char *notebook_id,
                                        const char *src_folder_path, const char *file_name,
                                        const char *dest_folder_path, const char *new_name,
                                        char **out_file_id) {
  if (!context || !notebook_id || !src_folder_path || !file_name || !dest_folder_path ||
      !out_file_id) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::FolderManager *folder_manager = notebook->GetFolderManager();
    if (!folder_manager) {
      ctx->last_error = "FolderManager not available";
      return VXCORE_ERR_INVALID_STATE;
    }
    std::string target_name = new_name ? new_name : "";
    std::string file_id;

    VxCoreError error = folder_manager->CopyFile(src_folder_path, file_name, dest_folder_path,
                                                 target_name, file_id);
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
