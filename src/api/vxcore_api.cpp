#include "vxcore/vxcore.h"

#include "core/context.h"

#include <stdlib.h>
#include <string.h>

VXCORE_API VxCoreVersion vxcore_get_version(void) {
  VxCoreVersion version = {0, 1, 0};
  return version;
}

VXCORE_API const char *vxcore_get_version_string(void) { return "0.1.0"; }

VXCORE_API const char *vxcore_error_message(VxCoreError error) {
  switch (error) {
  case VXCORE_OK:
    return "Success";
  case VXCORE_ERR_INVALID_PARAM:
    return "Invalid parameter";
  case VXCORE_ERR_NULL_POINTER:
    return "Null pointer";
  case VXCORE_ERR_OUT_OF_MEMORY:
    return "Out of memory";
  case VXCORE_ERR_NOT_FOUND:
    return "Not found";
  case VXCORE_ERR_ALREADY_EXISTS:
    return "Already exists";
  case VXCORE_ERR_IO:
    return "I/O error";
  case VXCORE_ERR_DATABASE:
    return "Database error";
  case VXCORE_ERR_JSON_PARSE:
    return "JSON parse error";
  case VXCORE_ERR_JSON_SERIALIZE:
    return "JSON serialize error";
  case VXCORE_ERR_INVALID_STATE:
    return "Invalid state";
  case VXCORE_ERR_NOT_INITIALIZED:
    return "Not initialized";
  case VXCORE_ERR_ALREADY_INITIALIZED:
    return "Already initialized";
  case VXCORE_ERR_PERMISSION_DENIED:
    return "Permission denied";
  case VXCORE_ERR_UNSUPPORTED:
    return "Unsupported operation";
  default:
    return "Unknown error";
  }
}

VXCORE_API VxCoreError vxcore_context_create(const char *config_json,
                                             VxCoreContextHandle *out_context) {
  (void)config_json;
  if (!out_context) {
    return VXCORE_ERR_NULL_POINTER;
  }

  try {
    auto *ctx = new vxcore::VxCoreContext();
    ctx->config_manager = std::make_unique<vxcore::ConfigManager>();

    VxCoreError err = ctx->config_manager->loadConfigs();
    if (err != VXCORE_OK) {
      delete ctx;
      return err;
    }

    *out_context = reinterpret_cast<VxCoreContextHandle>(ctx);
    return VXCORE_OK;
  } catch (...) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  }
}

VXCORE_API void vxcore_context_destroy(VxCoreContextHandle context) {
  if (context) {
    auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
    delete ctx;
  }
}

VXCORE_API VxCoreError vxcore_context_get_last_error(VxCoreContextHandle context,
                                                     const char **out_message) {
  if (!context || !out_message) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (ctx->last_error.empty()) {
    *out_message = "No error";
  } else {
    *out_message = ctx->last_error.c_str();
  }
  return VXCORE_OK;
}

VXCORE_API VxCoreError vxcore_notebook_open(VxCoreContextHandle context, const char *path,
                                            const char *options_json,
                                            VxCoreNotebookHandle *out_notebook) {
  (void)context;
  (void)path;
  (void)options_json;
  if (!out_notebook) {
    return VXCORE_ERR_NULL_POINTER;
  }
  *out_notebook = nullptr;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_notebook_close(VxCoreContextHandle context,
                                             VxCoreNotebookHandle notebook) {
  (void)context;
  (void)notebook;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_notebook_create(VxCoreContextHandle context, const char *path,
                                              const char *config_json,
                                              VxCoreNotebookHandle *out_notebook) {
  (void)context;
  (void)path;
  (void)config_json;
  if (!out_notebook) {
    return VXCORE_ERR_NULL_POINTER;
  }
  *out_notebook = nullptr;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_notebook_get_info(VxCoreContextHandle context,
                                                VxCoreNotebookHandle notebook,
                                                char **out_info_json) {
  (void)context;
  (void)notebook;
  if (!out_info_json) {
    return VXCORE_ERR_NULL_POINTER;
  }
  *out_info_json = nullptr;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_note_create(VxCoreContextHandle context,
                                          VxCoreNotebookHandle notebook, const char *params_json,
                                          VxCoreNoteHandle *out_note) {
  (void)context;
  (void)notebook;
  (void)params_json;
  if (!out_note) {
    return VXCORE_ERR_NULL_POINTER;
  }
  *out_note = nullptr;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_note_open(VxCoreContextHandle context, VxCoreNotebookHandle notebook,
                                        const char *note_id, VxCoreNoteHandle *out_note) {
  (void)context;
  (void)notebook;
  (void)note_id;
  if (!out_note) {
    return VXCORE_ERR_NULL_POINTER;
  }
  *out_note = nullptr;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_note_get_info(VxCoreContextHandle context, VxCoreNoteHandle note,
                                            char **out_info_json) {
  (void)context;
  (void)note;
  if (!out_info_json) {
    return VXCORE_ERR_NULL_POINTER;
  }
  *out_info_json = nullptr;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_note_update(VxCoreContextHandle context, VxCoreNoteHandle note,
                                          const char *update_json) {
  (void)context;
  (void)note;
  (void)update_json;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_note_delete(VxCoreContextHandle context,
                                          VxCoreNotebookHandle notebook, const char *note_id) {
  (void)context;
  (void)notebook;
  (void)note_id;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_note_move(VxCoreContextHandle context, VxCoreNotebookHandle notebook,
                                        const char *note_id, const char *new_path) {
  (void)context;
  (void)notebook;
  (void)note_id;
  (void)new_path;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_note_list(VxCoreContextHandle context, VxCoreNotebookHandle notebook,
                                        const char *filter_json, char **out_notes_json) {
  (void)context;
  (void)notebook;
  (void)filter_json;
  if (!out_notes_json) {
    return VXCORE_ERR_NULL_POINTER;
  }
  *out_notes_json = nullptr;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_tag_create(VxCoreContextHandle context, VxCoreNotebookHandle notebook,
                                         const char *tag_name, VxCoreTagHandle *out_tag) {
  (void)context;
  (void)notebook;
  (void)tag_name;
  if (!out_tag) {
    return VXCORE_ERR_NULL_POINTER;
  }
  *out_tag = nullptr;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_tag_delete(VxCoreContextHandle context, VxCoreNotebookHandle notebook,
                                         const char *tag_name) {
  (void)context;
  (void)notebook;
  (void)tag_name;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_tag_list(VxCoreContextHandle context, VxCoreNotebookHandle notebook,
                                       char **out_tags_json) {
  (void)context;
  (void)notebook;
  if (!out_tags_json) {
    return VXCORE_ERR_NULL_POINTER;
  }
  *out_tags_json = nullptr;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_note_add_tag(VxCoreContextHandle context, VxCoreNoteHandle note,
                                           const char *tag_name) {
  (void)context;
  (void)note;
  (void)tag_name;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_note_remove_tag(VxCoreContextHandle context, VxCoreNoteHandle note,
                                              const char *tag_name) {
  (void)context;
  (void)note;
  (void)tag_name;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_search(VxCoreContextHandle context, VxCoreNotebookHandle notebook,
                                     const char *query_json, VxCoreSearchResultHandle *out_result) {
  (void)context;
  (void)notebook;
  (void)query_json;
  if (!out_result) {
    return VXCORE_ERR_NULL_POINTER;
  }
  *out_result = nullptr;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API VxCoreError vxcore_search_get_results(VxCoreContextHandle context,
                                                 VxCoreSearchResultHandle result,
                                                 char **out_results_json) {
  (void)context;
  (void)result;
  if (!out_results_json) {
    return VXCORE_ERR_NULL_POINTER;
  }
  *out_results_json = nullptr;
  return VXCORE_ERR_UNSUPPORTED;
}

VXCORE_API void vxcore_search_result_destroy(VxCoreSearchResultHandle result) { (void)result; }

VXCORE_API void vxcore_string_free(char *str) {
  if (str) {
    free(str);
  }
}
