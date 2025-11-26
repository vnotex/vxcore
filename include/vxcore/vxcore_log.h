#ifndef VXCORE_LOG_H
#define VXCORE_LOG_H

#include "vxcore_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  VXCORE_LOG_LEVEL_TRACE = 0,
  VXCORE_LOG_LEVEL_DEBUG = 1,
  VXCORE_LOG_LEVEL_INFO = 2,
  VXCORE_LOG_LEVEL_WARN = 3,
  VXCORE_LOG_LEVEL_ERROR = 4,
  VXCORE_LOG_LEVEL_FATAL = 5,
  VXCORE_LOG_LEVEL_OFF = 6
} VxCoreLogLevel;

VXCORE_API VxCoreError vxcore_log_set_level(VxCoreLogLevel level);

VXCORE_API VxCoreError vxcore_log_set_file(const char *path);

VXCORE_API VxCoreError vxcore_log_enable_console(int enable);

#ifdef __cplusplus
}
#endif

#endif
