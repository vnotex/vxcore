#include "vxcore/vxcore.h"

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
