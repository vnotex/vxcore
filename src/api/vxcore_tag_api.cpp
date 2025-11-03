#include "vxcore/vxcore.h"

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
