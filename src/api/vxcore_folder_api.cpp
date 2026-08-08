#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>

#include "api/api_utils.h"
#include "core/context.h"
#include "core/folder.h"
#include "core/folder_manager.h"
#include "core/metadata_store.h"
#include "core/notebook.h"
#include "core/notebook_manager.h"
#include "utils/file_utils.h"
#include "vxcore/notebook_json_keys.h"
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

VXCORE_API VxCoreError vxcore_file_update_attachments(VxCoreContextHandle context,
                                                      const char *notebook_id,
                                                      const char *file_path,
                                                      const char *attachments_json) {
  if (!context || !notebook_id || !file_path || !attachments_json) {
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
    return folder_manager->UpdateFileAttachments(file_path, attachments_json);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_file_add_attachment(VxCoreContextHandle context,
                                                  const char *notebook_id,
                                                  const char *file_path,
                                                  const char *attachment) {
  if (!context || !notebook_id || !file_path || !attachment) {
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
    return folder_manager->AddFileAttachment(file_path, attachment);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_file_delete_attachment(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     const char *file_path,
                                                     const char *attachment) {
  if (!context || !notebook_id || !file_path || !attachment) {
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
    return folder_manager->DeleteFileAttachment(file_path, attachment);
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
                                                   const char *notebook_id, const char *folder_path,
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

    // Per-child existence reporting is a BUNDLED-only, OUTPUT-ONLY concern. Raw
    // notebooks self-heal on read and must keep byte-identical listing output.
    const bool is_bundled = notebook->GetType() == vxcore::NotebookType::Bundled;

    // Folder-missing guard (bundled only): if the listed folder's own content
    // dir is gone from disk, surface it as a missing node rather than emitting a
    // phantom listing. Metadata (vx.json) may still exist, so ListFolderContents
    // above succeeds; the disk check is what distinguishes a phantom folder.
    if (is_bundled && !folder_manager->NodeContentExistsOnDisk(path, /*is_folder=*/true)) {
      return VXCORE_ERR_NODE_NOT_EXISTS;
    }

    // Build JSON response
    nlohmann::json result;
    nlohmann::json files_json = nlohmann::json::array();
    nlohmann::json folders_json = nlohmann::json::array();

    for (const auto &file : contents.files) {
      nlohmann::json file_json = file.ToJsonWithType();
      // Mark (do NOT filter) children whose on-disk content is gone, so the UI
      // can gray + offer removal. Transient: never persisted to vx.json.
      if (is_bundled) {
        const std::string child_path = vxcore::ConcatenatePaths(path, file.name);
        file_json[vxcore::kJsonKeyNodeExists] =
            folder_manager->NodeContentExistsOnDisk(child_path, /*is_folder=*/false);
      }
      files_json.push_back(file_json);
    }

    for (const auto &folder : contents.folders) {
      nlohmann::json folder_json = folder.ToJson();
      if (is_bundled) {
        const std::string child_path = vxcore::ConcatenatePaths(path, folder.name);
        folder_json[vxcore::kJsonKeyNodeExists] =
            folder_manager->NodeContentExistsOnDisk(child_path, /*is_folder=*/true);
      }
      folders_json.push_back(folder_json);
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
                                                   const char *notebook_id, const char *folder_path,
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
                                                  const char *notebook_id, const char *node_id,
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

VXCORE_API VxCoreError vxcore_node_resolve_by_id(VxCoreContextHandle context, const char *node_id,
                                                 char **out_notebook_id, char **out_relative_path) {
  if (!context || !node_id || !out_notebook_id || !out_relative_path) {
    return VXCORE_ERR_NULL_POINTER;
  }

  *out_notebook_id = nullptr;
  *out_relative_path = nullptr;

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    std::string notebook_id;
    std::string relative_path;
    VxCoreError err = ctx->notebook_manager->ResolveNodeById(node_id, notebook_id, relative_path);

    if (err != VXCORE_OK) {
      ctx->last_error = "Node not found in any open notebook";
      return err;
    }

    char *id_copy = vxcore_strdup(notebook_id.c_str());
    if (!id_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    char *path_copy = vxcore_strdup(relative_path.c_str());
    if (!path_copy) {
      free(id_copy);
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_notebook_id = id_copy;
    *out_relative_path = path_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error resolving node by ID";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_node_get_attachments_folder(VxCoreContextHandle context,
                                                          const char *notebook_id,
                                                          const char *file_path, char **out_path) {
  if (!context || !notebook_id || !file_path || !out_path) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  *out_path = nullptr;

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

    const vxcore::FileRecord *record = nullptr;
    VxCoreError error = folder_manager->GetFileInfo(file_path, &record);
    if (error != VXCORE_OK) {
      return error;
    }

    std::string path = folder_manager->GetAssetsFolder(file_path);
    if (path.empty()) {
      ctx->last_error = "File not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    *out_path = vxcore_strdup(path.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_node_list_attachments(VxCoreContextHandle context,
                                                    const char *notebook_id, const char *file_path,
                                                    char **out_attachments_json) {
  if (!context || !notebook_id || !file_path || !out_attachments_json) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  *out_attachments_json = nullptr;

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

    std::string attachments_json;
    VxCoreError error = folder_manager->GetFileAttachments(file_path, attachments_json);
    if (error != VXCORE_OK) {
      return error;
    }

    // Extract just the filenames from relative paths (e.g., "vx_assets/uuid/doc.pdf" -> "doc.pdf")
    try {
      nlohmann::json j = nlohmann::json::parse(attachments_json);
      if (j.is_array()) {
        nlohmann::json filenames = nlohmann::json::array();
        for (const auto &rel_path : j) {
          std::string path_str = rel_path.get<std::string>();
          filenames.push_back(vxcore::PathFilename(path_str));
        }
        *out_attachments_json = vxcore_strdup(filenames.dump().c_str());
      } else {
        *out_attachments_json = vxcore_strdup(attachments_json.c_str());
      }
    } catch (const std::exception &) {
      *out_attachments_json = vxcore_strdup(attachments_json.c_str());
    }
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_folder_get_available_name(VxCoreContextHandle context,
                                                        const char *notebook_id,
                                                        const char *folder_path,
                                                        const char *new_name,
                                                        char **out_available_name) {
  if (!context || !notebook_id || !new_name || !out_available_name) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  *out_available_name = nullptr;

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
    std::string available_name;

    VxCoreError error = folder_manager->GetAvailableName(path, new_name, available_name);
    if (error != VXCORE_OK) {
      return error;
    }

    *out_available_name = vxcore_strdup(available_name.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_folder_set_children_order(VxCoreContextHandle context,
                                                        const char *notebook_id,
                                                        const char *folder_path,
                                                        const char *ordered_json) {
  if (!context || !notebook_id || !folder_path || !ordered_json) {
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

    // "" or "." → notebook root, matching vxcore_folder_list_children
    // convention at line 307 above.
    std::string path = std::string(folder_path).empty() ? "." : folder_path;
    return folder_manager->SetChildrenOrder(path, ordered_json);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

namespace {

// A path component is safe iff it is a single, non-traversing name.
bool IsSafePathComponent(const std::string &component) {
  if (component.empty() || component == "." || component == "..") {
    return false;
  }
  return component.find('/') == std::string::npos && component.find('\\') == std::string::npos;
}

// Load and parse a folder's vx.json. Returns VXCORE_ERR_NOT_FOUND when the
// file is absent, VXCORE_ERR_JSON_PARSE when it does not parse into an object.
VxCoreError LoadFolderConfigJson(const std::filesystem::path &config_dir,
                                 nlohmann::json &out_json) {
  const std::filesystem::path config_file = config_dir / "vx.json";
  std::error_code ec;
  if (!std::filesystem::is_regular_file(config_file, ec) || ec) {
    return VXCORE_ERR_NOT_FOUND;
  }
  VxCoreError err = vxcore::LoadJsonFile(config_file, out_json);
  if (err != VXCORE_OK) {
    return VXCORE_ERR_JSON_PARSE;
  }
  if (!out_json.is_object()) {
    return VXCORE_ERR_JSON_PARSE;
  }
  return VXCORE_OK;
}

// Count how many times @name appears in a folder config's "folders" array.
size_t CountFolderChild(const nlohmann::json &config, const std::string &name) {
  if (!config.contains(vxcore::kJsonKeyFolders) ||
      !config[vxcore::kJsonKeyFolders].is_array()) {
    return 0;
  }
  size_t count = 0;
  for (const auto &entry : config[vxcore::kJsonKeyFolders]) {
    if (entry.is_string() && entry.get<std::string>() == name) {
      ++count;
    }
  }
  return count;
}

}  // namespace

VXCORE_API VxCoreError vxcore_folder_get_share_paths(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     const char *folder_path,
                                                     char **out_notebook_root,
                                                     char **out_content_root,
                                                     char **out_metadata_root) {
  // Initialize every output before any validation so a failing caller never
  // frees an uninitialized pointer.
  if (out_notebook_root) {
    *out_notebook_root = nullptr;
  }
  if (out_content_root) {
    *out_content_root = nullptr;
  }
  if (out_metadata_root) {
    *out_metadata_root = nullptr;
  }

  if (!context || !notebook_id || !folder_path || !out_notebook_root || !out_content_root ||
      !out_metadata_root) {
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
      ctx->last_error = "Folder sharing requires a bundled notebook";
      return VXCORE_ERR_UNSUPPORTED;
    }

    const std::string raw_path(folder_path);
    if (raw_path.empty()) {
      ctx->last_error = "Cannot share the notebook root";
      return VXCORE_ERR_INVALID_PARAM;
    }
    // Reject anything rooted: drive letters / UNC roots (has_root_name) AND
    // leading separators (has_root_directory). On Windows "/Alpha" is
    // technically "relative" to std::filesystem because it lacks a root NAME,
    // so is_relative() alone is not a sufficient guard.
    {
      const std::filesystem::path requested = vxcore::PathFromUtf8(raw_path);
      if (requested.has_root_name() || requested.has_root_directory() ||
          !requested.is_relative()) {
        ctx->last_error = "Folder path must be relative to the notebook root";
        return VXCORE_ERR_INVALID_PARAM;
      }
    }

    const std::string clean_path = vxcore::CleanPath(raw_path);
    if (clean_path == "." || clean_path.empty()) {
      ctx->last_error = "Cannot share the notebook root";
      return VXCORE_ERR_INVALID_PARAM;
    }

    // Reject "." / ".." as WRITTEN, before normalization. CleanPath() would
    // otherwise collapse "Projects/../Alpha" to "Alpha" and silently accept a
    // request the contract declares invalid.
    {
      std::string normalized_separators = raw_path;
      for (char &ch : normalized_separators) {
        if (ch == '\\') {
          ch = '/';
        }
      }
      for (const auto &component : vxcore::SplitPathComponents(normalized_separators)) {
        if (component == "." || component == "..") {
          ctx->last_error = "Folder path must not contain \".\" or \"..\" components";
          return VXCORE_ERR_INVALID_PARAM;
        }
      }
    }

    const std::vector<std::string> components = vxcore::SplitPathComponents(clean_path);
    if (components.empty()) {
      ctx->last_error = "Cannot share the notebook root";
      return VXCORE_ERR_INVALID_PARAM;
    }
    for (const auto &component : components) {
      if (!IsSafePathComponent(component)) {
        ctx->last_error = "Folder path contains an unsafe component: " + component;
        return VXCORE_ERR_INVALID_PARAM;
      }
    }

    const std::filesystem::path notebook_root =
        vxcore::PathFromUtf8(notebook->GetRootFolder());
    const std::filesystem::path metadata_contents =
        vxcore::PathFromUtf8(notebook->GetMetadataFolder()) / "contents";

    // Walk every component from the notebook root, proving full index
    // reachability. An orphan subtree (physical + metadata present, but an
    // ancestor edge missing) must be rejected.
    nlohmann::json parent_config;
    VxCoreError err = LoadFolderConfigJson(metadata_contents, parent_config);
    if (err != VXCORE_OK) {
      ctx->last_error = "Notebook root folder metadata is missing or malformed";
      return err;
    }

    std::filesystem::path config_dir = metadata_contents;
    std::filesystem::path content_dir = notebook_root;
    for (const auto &component : components) {
      const size_t occurrences = CountFolderChild(parent_config, component);
      if (occurrences != 1) {
        ctx->last_error = occurrences == 0
                              ? ("Folder is not indexed by its parent: " + component)
                              : ("Folder is listed more than once by its parent: " + component);
        return VXCORE_ERR_NOT_FOUND;
      }

      config_dir /= vxcore::PathFromUtf8(component);
      content_dir /= vxcore::PathFromUtf8(component);

      nlohmann::json child_config;
      err = LoadFolderConfigJson(config_dir, child_config);
      if (err != VXCORE_OK) {
        ctx->last_error = "Folder metadata is missing or malformed: " + component;
        return err;
      }

      if (!child_config.contains(vxcore::kJsonKeyName) ||
          !child_config[vxcore::kJsonKeyName].is_string() ||
          child_config[vxcore::kJsonKeyName].get<std::string>() != component) {
        ctx->last_error = "Folder metadata name does not match its path component: " + component;
        return VXCORE_ERR_INVALID_STATE;
      }

      std::error_code ec;
      // symlink_status does NOT follow the link, so a directory symlink (and,
      // on Windows/MSVC, a junction) is reported as a symlink here rather than
      // as the directory it points at. Following one would let the share walk
      // out of the notebook entirely.
      const auto sym = std::filesystem::symlink_status(content_dir, ec);
      if (ec) {
        ctx->last_error = "Folder content is missing on disk: " + component;
        return VXCORE_ERR_NODE_NOT_EXISTS;
      }
      if (std::filesystem::is_symlink(sym)) {
        ctx->last_error = "Folder content is a symbolic link or reparse point: " + component;
        return VXCORE_ERR_UNSUPPORTED;
      }
      if (!std::filesystem::is_directory(content_dir, ec) || ec) {
        ctx->last_error = "Folder content is missing on disk: " + component;
        return VXCORE_ERR_NODE_NOT_EXISTS;
      }

      parent_config = std::move(child_config);
    }

    // All validation passed; allocate the outputs, unwinding on OOM.
    const std::string notebook_root_utf8 = vxcore::PathToUtf8(notebook_root);
    const std::string content_root_utf8 = vxcore::PathToUtf8(content_dir);
    const std::string metadata_root_utf8 = vxcore::PathToUtf8(config_dir);

    char *notebook_root_out = vxcore_strdup(notebook_root_utf8.c_str());
    if (!notebook_root_out) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }
    char *content_root_out = vxcore_strdup(content_root_utf8.c_str());
    if (!content_root_out) {
      free(notebook_root_out);
      return VXCORE_ERR_OUT_OF_MEMORY;
    }
    char *metadata_root_out = vxcore_strdup(metadata_root_utf8.c_str());
    if (!metadata_root_out) {
      free(notebook_root_out);
      free(content_root_out);
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_notebook_root = notebook_root_out;
    *out_content_root = content_root_out;
    *out_metadata_root = metadata_root_out;
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_file_peek(VxCoreContextHandle context, const char *notebook_id,
                                        const char *file_path, size_t max_bytes,
                                        char **out_content) {
  if (!context || !notebook_id || !file_path || !out_content) {
    return VXCORE_ERR_INVALID_PARAM;
  }

  *out_content = nullptr;

  vxcore::VxCoreContext *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    vxcore::Notebook *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    std::filesystem::path abs_path = vxcore::PathFromUtf8(notebook->GetRootFolder()) / vxcore::PathFromUtf8(file_path);
    if (!std::filesystem::exists(abs_path) || !std::filesystem::is_regular_file(abs_path)) {
      ctx->last_error = "File not found: " + std::string(file_path);
      return VXCORE_ERR_NOT_FOUND;
    }

    std::string content;
    VxCoreError error = vxcore::ReadFileHead(abs_path, max_bytes, content);
    if (error != VXCORE_OK) {
      ctx->last_error = "Failed to read file";
      return error;
    }

    *out_content = vxcore_strdup(content.c_str());
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_node_update_timestamps(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     const char *node_path, int64_t created_utc,
                                                     int64_t modified_utc) {
  if (!context || !notebook_id || !node_path) {
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

    return folder_manager->UpdateNodeTimestamps(node_path, created_utc, modified_utc);
  } catch (const std::exception &e) {
    ctx->last_error = std::string("Exception: ") + e.what();
    return VXCORE_ERR_UNKNOWN;
  }
}
