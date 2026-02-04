#ifndef VXCORE_H
#define VXCORE_H

#include "vxcore_types.h"

#ifdef __cplusplus
extern "C" {
#endif

VXCORE_API VxCoreVersion vxcore_get_version(void);

VXCORE_API const char *vxcore_get_version_string(void);

VXCORE_API const char *vxcore_error_message(VxCoreError error);

VXCORE_API void vxcore_set_test_mode(int enabled);

VXCORE_API void vxcore_set_app_info(const char *org_name, const char *app_name);

VXCORE_API VxCoreError vxcore_context_create(const char *config_json,
                                             VxCoreContextHandle *out_context);

VXCORE_API void vxcore_context_destroy(VxCoreContextHandle context);

VXCORE_API VxCoreError vxcore_context_get_last_error(VxCoreContextHandle context,
                                                     const char **out_message);

VXCORE_API VxCoreError vxcore_context_get_config_path(VxCoreContextHandle context, char **out_path);

VXCORE_API VxCoreError vxcore_context_get_session_config_path(VxCoreContextHandle context,
                                                              char **out_path);

VXCORE_API VxCoreError vxcore_context_get_config(VxCoreContextHandle context, char **out_json);

VXCORE_API VxCoreError vxcore_context_get_session_config(VxCoreContextHandle context,
                                                         char **out_json);

VXCORE_API VxCoreError vxcore_context_get_config_by_name(VxCoreContextHandle context,
                                                         VxCoreDataLocation location,
                                                         const char *base_name, char **out_json);

VXCORE_API VxCoreError vxcore_context_update_config_by_name(VxCoreContextHandle context,
                                                            VxCoreDataLocation location,
                                                            const char *base_name,
                                                            const char *json);

VXCORE_API VxCoreError vxcore_notebook_create(VxCoreContextHandle context, const char *path,
                                              const char *config_json, VxCoreNotebookType type,
                                              char **out_notebook_id);

VXCORE_API VxCoreError vxcore_notebook_open(VxCoreContextHandle context, const char *path,
                                            char **out_notebook_id);

VXCORE_API VxCoreError vxcore_notebook_close(VxCoreContextHandle context, const char *notebook_id);

VXCORE_API VxCoreError vxcore_notebook_list(VxCoreContextHandle context, char **out_notebooks_json);

VXCORE_API VxCoreError vxcore_notebook_get_config(VxCoreContextHandle context,
                                                  const char *notebook_id, char **out_config_json);

VXCORE_API VxCoreError vxcore_notebook_update_config(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     const char *config_json);

// Rebuilds the metadata cache for the notebook from ground truth (vx.json files).
// Use this when the cache seems out of sync or corrupted.
// The cache uses lazy sync by default - this forces a full rebuild.
VXCORE_API VxCoreError vxcore_notebook_rebuild_cache(VxCoreContextHandle context,
                                                     const char *notebook_id);

VXCORE_API VxCoreError vxcore_folder_create(VxCoreContextHandle context, const char *notebook_id,
                                            const char *parent_path, const char *folder_name,
                                            char **out_folder_id);

VXCORE_API VxCoreError vxcore_folder_create_path(VxCoreContextHandle context,
                                                 const char *notebook_id, const char *folder_path,
                                                 char **out_folder_id);

VXCORE_API VxCoreError vxcore_folder_delete(VxCoreContextHandle context, const char *notebook_id,
                                            const char *folder_path);

VXCORE_API VxCoreError vxcore_folder_get_config(VxCoreContextHandle context,
                                                const char *notebook_id, const char *folder_path,
                                                char **out_config_json);

VXCORE_API VxCoreError vxcore_folder_update_metadata(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     const char *folder_path,
                                                     const char *metadata_json);

VXCORE_API VxCoreError vxcore_folder_get_metadata(VxCoreContextHandle context,
                                                  const char *notebook_id, const char *folder_path,
                                                  char **out_metadata_json);

VXCORE_API VxCoreError vxcore_folder_rename(VxCoreContextHandle context, const char *notebook_id,
                                            const char *folder_path, const char *new_name);

VXCORE_API VxCoreError vxcore_folder_move(VxCoreContextHandle context, const char *notebook_id,
                                          const char *src_path, const char *dest_parent_path);

VXCORE_API VxCoreError vxcore_folder_copy(VxCoreContextHandle context, const char *notebook_id,
                                          const char *src_path, const char *dest_parent_path,
                                          const char *new_name, char **out_folder_id);

VXCORE_API VxCoreError vxcore_file_create(VxCoreContextHandle context, const char *notebook_id,
                                          const char *folder_path, const char *file_name,
                                          char **out_file_id);

VXCORE_API VxCoreError vxcore_file_delete(VxCoreContextHandle context, const char *notebook_id,
                                          const char *file_path);

VXCORE_API VxCoreError vxcore_file_get_info(VxCoreContextHandle context, const char *notebook_id,
                                            const char *file_path, char **out_file_info_json);

VXCORE_API VxCoreError vxcore_file_get_metadata(VxCoreContextHandle context,
                                                const char *notebook_id, const char *file_path,
                                                char **out_metadata_json);

VXCORE_API VxCoreError vxcore_file_update_metadata(VxCoreContextHandle context,
                                                   const char *notebook_id, const char *file_path,
                                                   const char *metadata_json);

VXCORE_API VxCoreError vxcore_file_rename(VxCoreContextHandle context, const char *notebook_id,
                                          const char *file_path, const char *new_name);

VXCORE_API VxCoreError vxcore_file_move(VxCoreContextHandle context, const char *notebook_id,
                                        const char *src_file_path, const char *dest_folder_path);

VXCORE_API VxCoreError vxcore_file_copy(VxCoreContextHandle context, const char *notebook_id,
                                        const char *src_file_path, const char *dest_folder_path,
                                        const char *new_name, char **out_file_id);

VXCORE_API VxCoreError vxcore_file_update_tags(VxCoreContextHandle context, const char *notebook_id,
                                               const char *file_path, const char *tags_json);

VXCORE_API VxCoreError vxcore_file_tag(VxCoreContextHandle context, const char *notebook_id,
                                       const char *file_path, const char *tag_name);

VXCORE_API VxCoreError vxcore_file_untag(VxCoreContextHandle context, const char *notebook_id,
                                         const char *file_path, const char *tag_name);

VXCORE_API VxCoreError vxcore_tag_create(VxCoreContextHandle context, const char *notebook_id,
                                         const char *tag_name);

VXCORE_API VxCoreError vxcore_tag_create_path(VxCoreContextHandle context, const char *notebook_id,
                                              const char *tag_path);

VXCORE_API VxCoreError vxcore_tag_delete(VxCoreContextHandle context, const char *notebook_id,
                                         const char *tag_name);

VXCORE_API VxCoreError vxcore_tag_list(VxCoreContextHandle context, const char *notebook_id,
                                       char **out_tags_json);

VXCORE_API VxCoreError vxcore_tag_move(VxCoreContextHandle context, const char *notebook_id,
                                       const char *tag_name, const char *parent_tag);

VXCORE_API VxCoreError vxcore_search_files(VxCoreContextHandle context, const char *notebook_id,
                                           const char *query_json, const char *input_files_json,
                                           char **out_results_json);

VXCORE_API VxCoreError vxcore_search_content(VxCoreContextHandle context, const char *notebook_id,
                                             const char *query_json, const char *input_files_json,
                                             char **out_results_json);

VXCORE_API VxCoreError vxcore_search_by_tags(VxCoreContextHandle context, const char *notebook_id,
                                             const char *query_json, const char *input_files_json,
                                             char **out_results_json);

VXCORE_API void vxcore_string_free(char *str);

#ifdef __cplusplus
}
#endif

#endif
