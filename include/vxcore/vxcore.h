#ifndef VXCORE_H
#define VXCORE_H

#include "vxcore_events.h"
#include "vxcore_types.h"

#ifdef __cplusplus
extern "C" {
#endif

VXCORE_API VxCoreVersion vxcore_get_version(void);

VXCORE_API const char *vxcore_get_version_string(void);

VXCORE_API const char *vxcore_error_message(VxCoreError error);

VXCORE_API VxCoreError vxcore_context_create(const char *config_json,
                                             VxCoreContextHandle *out_context);

VXCORE_API void vxcore_context_destroy(VxCoreContextHandle context);

VXCORE_API VxCoreError vxcore_context_get_last_error(VxCoreContextHandle context,
                                                     const char **out_message);

VXCORE_API VxCoreError vxcore_notebook_open(VxCoreContextHandle context, const char *path,
                                            const char *options_json,
                                            VxCoreNotebookHandle *out_notebook);

VXCORE_API VxCoreError vxcore_notebook_close(VxCoreContextHandle context,
                                             VxCoreNotebookHandle notebook);

VXCORE_API VxCoreError vxcore_notebook_create(VxCoreContextHandle context, const char *path,
                                              const char *config_json,
                                              VxCoreNotebookHandle *out_notebook);

VXCORE_API VxCoreError vxcore_notebook_get_info(VxCoreContextHandle context,
                                                VxCoreNotebookHandle notebook,
                                                char **out_info_json);

VXCORE_API VxCoreError vxcore_note_create(VxCoreContextHandle context,
                                          VxCoreNotebookHandle notebook, const char *params_json,
                                          VxCoreNoteHandle *out_note);

VXCORE_API VxCoreError vxcore_note_open(VxCoreContextHandle context, VxCoreNotebookHandle notebook,
                                        const char *note_id, VxCoreNoteHandle *out_note);

VXCORE_API VxCoreError vxcore_note_get_info(VxCoreContextHandle context, VxCoreNoteHandle note,
                                            char **out_info_json);

VXCORE_API VxCoreError vxcore_note_update(VxCoreContextHandle context, VxCoreNoteHandle note,
                                          const char *update_json);

VXCORE_API VxCoreError vxcore_note_delete(VxCoreContextHandle context,
                                          VxCoreNotebookHandle notebook, const char *note_id);

VXCORE_API VxCoreError vxcore_note_move(VxCoreContextHandle context, VxCoreNotebookHandle notebook,
                                        const char *note_id, const char *new_path);

VXCORE_API VxCoreError vxcore_note_list(VxCoreContextHandle context, VxCoreNotebookHandle notebook,
                                        const char *filter_json, char **out_notes_json);

VXCORE_API VxCoreError vxcore_tag_create(VxCoreContextHandle context, VxCoreNotebookHandle notebook,
                                         const char *tag_name, VxCoreTagHandle *out_tag);

VXCORE_API VxCoreError vxcore_tag_delete(VxCoreContextHandle context, VxCoreNotebookHandle notebook,
                                         const char *tag_name);

VXCORE_API VxCoreError vxcore_tag_list(VxCoreContextHandle context, VxCoreNotebookHandle notebook,
                                       char **out_tags_json);

VXCORE_API VxCoreError vxcore_note_add_tag(VxCoreContextHandle context, VxCoreNoteHandle note,
                                           const char *tag_name);

VXCORE_API VxCoreError vxcore_note_remove_tag(VxCoreContextHandle context, VxCoreNoteHandle note,
                                              const char *tag_name);

VXCORE_API VxCoreError vxcore_search(VxCoreContextHandle context, VxCoreNotebookHandle notebook,
                                     const char *query_json, VxCoreSearchResultHandle *out_result);

VXCORE_API VxCoreError vxcore_search_get_results(VxCoreContextHandle context,
                                                 VxCoreSearchResultHandle result,
                                                 char **out_results_json);

VXCORE_API void vxcore_search_result_destroy(VxCoreSearchResultHandle result);

VXCORE_API void vxcore_string_free(char *str);

#ifdef __cplusplus
}
#endif

#endif
