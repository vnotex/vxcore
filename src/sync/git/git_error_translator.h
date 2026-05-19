#ifndef VXCORE_SYNC_GIT_GIT_ERROR_TRANSLATOR_H
#define VXCORE_SYNC_GIT_GIT_ERROR_TRANSLATOR_H

#include "vxcore/vxcore_types.h"

namespace vxcore {

// T28: translate libgit2 return codes / error classes to VxCoreError. Always
// reads git_error_last() (and logs it) so the original libgit2 message is
// preserved in the log even when we collapse many klass values into a single
// VxCoreError. libgit2 v1.7.x has no GIT_ERROR_TCP / GIT_ERROR_NOTFOUND klass
// constants; we rely on the return-code constants (GIT_ENOTFOUND,
// GIT_EINVALIDSPEC) for those cases instead.
VxCoreError TranslateGitError(int git_rc);

}  // namespace vxcore

#endif  // VXCORE_SYNC_GIT_GIT_ERROR_TRANSLATOR_H
