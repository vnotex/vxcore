#include "api/api_utils.h"
#include "core/context.h"
#include "core/folder.h"
#include "core/folder_manager.h"
#include "core/metadata_store.h"
#include "core/notebook_manager.h"
#include "vxcore/vxcore.h"

#include <nlohmann/json.hpp>

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

VXCORE_API VxCoreError vxcore_folder_create_path(VxCoreContextHandle context,
                                                 const char *notebook_id, const char *folder_path,
                                                 char **out_folder_id) {
  if (!context || !notebook_id || !folder_path || !out_folder_id) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    std::string folder_id;
    VxCoreError error = notebook->GetFolderManager()->CreateFolderPath(folder_path, folder_id);
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

VXCORE_API VxCoreError vxcore_file_update_tags(VxCoreContextHandle context, const char *notebook_id,
                                               const char *file_path, const char *tags_json) {
  if (!context || !notebook_id || !file_path || !tags_json) {
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
    return folder_manager->UpdateFileTags(file_path, tags_json);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_file_tag(VxCoreContextHandle context, const char *notebook_id,
                                       const char *file_path, const char *tag_name) {
  if (!context || !notebook_id || !file_path || !tag_name) {
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
    return folder_manager->TagFile(file_path, tag_name);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_file_untag(VxCoreContextHandle context, const char *notebook_id,
                                         const char *file_path, const char *tag_name) {
  if (!context || !notebook_id || !file_path || !tag_name) {
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
    return folder_manager->UntagFile(file_path, tag_name);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_folder_list_children(VxCoreContextHandle context,
                                                   const char *notebook_id,
                                                   const char *folder_path,
                                                   char **out_children_json) {
  if (!context || !notebook_id || !out_children_json) {
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

    std::string path = folder_path ? folder_path : ".";
    vxcore::FolderManager::FolderContents contents;

    VxCoreError error = folder_manager->ListFolderContents(path, true, contents);
    if (error != VXCORE_OK) {
      return error;
    }

    // Build JSON response
    nlohmann::json result;
    nlohmann::json files_json = nlohmann::json::array();
    nlohmann::json folders_json = nlohmann::json::array();

    for (const auto &file : contents.files) {
      files_json.push_back(file.ToJsonWithType());
    }

    for (const auto &folder : contents.folders) {
      folders_json.push_back(folder.ToJson());
    }

    result["files"] = files_json;
    result["folders"] = folders_json;

    *out_children_json = vxcore_strdup(result.dump().c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_folder_list_external(VxCoreContextHandle context,
                                                   const char *notebook_id,
                                                   const char *folder_path,
                                                   char **out_external_json) {
  if (!context || !notebook_id || !out_external_json) {
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

    std::string path = folder_path ? folder_path : ".";
    vxcore::FolderManager::FolderContents contents;

    VxCoreError error = folder_manager->ListExternalNodes(path, contents);
    if (error != VXCORE_OK) {
      return error;
    }

    // Build JSON response
    nlohmann::json result;
    nlohmann::json files_json = nlohmann::json::array();
    nlohmann::json folders_json = nlohmann::json::array();

    for (const auto &file : contents.files) {
      // External files only have name (no ID)
      nlohmann::json file_json;
      file_json["name"] = file.name;
      file_json["type"] = "file";
      files_json.push_back(file_json);
    }

    for (const auto &folder : contents.folders) {
      // External folders only have name (no ID)
      nlohmann::json folder_json;
      folder_json["name"] = folder.name;
      folder_json["type"] = "folder";
      folders_json.push_back(folder_json);
    }

    result["files"] = files_json;
    result["folders"] = folders_json;

    *out_external_json = vxcore_strdup(result.dump().c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}


VXCORE_API VxCoreError vxcore_file_import(VxCoreContextHandle context, const char *notebook_id,
                                          const char *folder_path, const char *external_file_path,
                                          char **out_file_id) {
  if (!context || !notebook_id || !folder_path || !external_file_path || !out_file_id) {
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
    VxCoreError error = folder_manager->ImportFile(folder_path, external_file_path, file_id);
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

VXCORE_API VxCoreError vxcore_folder_import(VxCoreContextHandle context, const char *notebook_id,
                                            const char *dest_folder_path,
                                            const char *external_folder_path,
                                            const char *suffix_allowlist, char **out_folder_id) {
  if (!context || !notebook_id || !dest_folder_path || !external_folder_path || !out_folder_id) {
    return VXCORE_ERR_NULL_POINTER;
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
    std::string allowlist = suffix_allowlist ? suffix_allowlist : "";
    VxCoreError error =
        folder_manager->ImportFolder(dest_folder_path, external_folder_path, allowlist, folder_id);
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

VXCORE_API VxCoreError vxcore_node_get_path_by_id(VxCoreContextHandle context,
                                                  const char *notebook_id,
                                                  const char *node_id,
                                                  char **out_path) {
  if (!context || !notebook_id || !node_id || !out_path) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  *out_path = nullptr;  // Initialize to nullptr

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    vxcore::MetadataStore *store = notebook->GetMetadataStore();
    if (!store) {
      ctx->last_error = "MetadataStore not available";
      return VXCORE_ERR_INVALID_STATE;
    }

    std::string path = store->GetNodePathById(node_id);
    if (path.empty()) {
      ctx->last_error = "Node not found: " + std::string(node_id);
      return VXCORE_ERR_NOT_FOUND;
    }

    *out_path = vxcore_strdup(path.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}