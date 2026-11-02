#ifndef VXCORE_MOBILE_H
#define VXCORE_MOBILE_H

#include "vxcore/vxcore.h"

#if defined(VXCORE_MOBILE_API) && VXCORE_MOBILE_API

#ifdef __cplusplus
extern "C" {
#endif

// Create an independent context with copied, absolute UTF-8 app/local roots.
// Both roots are validated before any directories or configuration are touched.
// Existing ancestors are canonicalized; equal/containing roots (including aliases)
// are invalid. Missing directories are created only after both roots pass validation.
// Null/empty/relative/invalid UTF-8 roots return INVALID_PARAM; inaccessible or
// non-directory ancestors return IO. A null output returns NULL_POINTER; otherwise
// *out_context is NULL on every failure. Allocation failure returns OUT_OF_MEMORY.
// config_json remains ignored, just as in vxcore_context_create. Explicit roots
// override desktop/portable/test-mode defaults for this context only. Callers must
// serialize context operations and protect roots against concurrent filesystem changes.
// Explicit-root contexts do not load vxsession.json or implicitly reopen notebooks,
// buffers, or workspaces. The embedding app owns recovery and explicit admission.
// User configuration still loads normally; default desktop context creation retains
// its existing session restoration. Later explicit operations may save session state.
VXCORE_API VxCoreError vxcore_context_create_with_paths(
    const char *config_json, const char *app_data_path, const char *local_data_path,
    VxCoreContextHandle *out_context);

#ifdef __cplusplus
}
#endif

#endif  // VXCORE_MOBILE_API

#endif  // VXCORE_MOBILE_H
