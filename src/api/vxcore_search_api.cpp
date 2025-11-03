#include "vxcore/vxcore.h"

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
