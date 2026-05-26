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

/**
 * Callback function type for custom log message handling.
 *
 * A custom handler REPLACES the default sinks (stderr/file logging), it is not
 * additive. Passing NULL as callback to vxcore_log_set_handler() restores the
 * default sinks.
 *
 * The callback is invoked while the internal logger mutex is held, so the
 * handler MUST be thread-safe and MUST NOT call back into vxcore APIs
 * synchronously — only queue work for async processing.
 *
 * The @p file and @p message pointers are valid only for the duration of the
 * call; do not retain them beyond return.
 *
 * @param level   Log level of the message (VXCORE_LOG_LEVEL_TRACE, etc.)
 * @param file    Source file name (__FILE__) where the log call originated
 * @param line    Source line number (__LINE__) where the log call originated
 * @param message Null-terminated log message text
 * @param userdata Opaque pointer supplied to vxcore_log_set_handler()
 */
typedef void (*VxCoreLogCallback)(VxCoreLogLevel level, const char *file, int line,
                                   const char *message, void *userdata);

/**
 * Set a custom handler for all log messages.
 *
 * @param callback Handler function, or NULL to restore default sinks
 * @param userdata Opaque pointer passed to the callback on every invocation
 * @return VXCORE_OK on success, error code otherwise
 */
VXCORE_API VxCoreError vxcore_log_set_handler(VxCoreLogCallback callback,
                                               void *userdata);

#ifdef __cplusplus
}
#endif

#endif
