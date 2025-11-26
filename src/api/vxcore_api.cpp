#include <stdlib.h>

#include "api_utils.h"
#include "core/config_manager.h"
#include "core/context.h"
#include "core/notebook_manager.h"
#include "utils/logger.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_log.h"

VXCORE_API VxCoreVersion vxcore_get_version(void) {
  VxCoreVersion version = {0, 1, 0};
  return version;
}

VXCORE_API const char *vxcore_get_version_string(void) { return "0.1.0"; }

VXCORE_API void vxcore_set_test_mode(int enabled) {
  vxcore::ConfigManager::SetTestMode(enabled != 0);
}

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

    VxCoreError err = ctx->config_manager->LoadConfigs();
    if (err != VXCORE_OK) {
      delete ctx;
      return err;
    }

    ctx->notebook_manager = std::make_unique<vxcore::NotebookManager>(
        ctx->config_manager->GetLocalDataPath(), &ctx->config_manager->GetSessionConfig());

    ctx->notebook_manager->SetSessionConfigUpdater([ctx]() {
      if (ctx && ctx->config_manager) {
        ctx->config_manager->SaveSessionConfig();
      }
    });

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

VXCORE_API VxCoreError vxcore_context_get_config_path(VxCoreContextHandle context,
                                                      char **out_path) {
  if (!context || !out_path) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->config_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    std::string path = ctx->config_manager->GetConfigPath();
    *out_path = vxcore_strdup(path.c_str());
    return VXCORE_OK;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_context_get_session_config_path(VxCoreContextHandle context,
                                                              char **out_path) {
  if (!context || !out_path) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->config_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    std::string path = ctx->config_manager->GetSessionConfigPath();
    *out_path = vxcore_strdup(path.c_str());
    return VXCORE_OK;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_context_get_config(VxCoreContextHandle context, char **out_json) {
  if (!context || !out_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->config_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    nlohmann::json json = ctx->config_manager->GetConfig().ToJson();
    std::string json_str = json.dump(2);
    *out_json = vxcore_strdup(json_str.c_str());
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_context_get_session_config(VxCoreContextHandle context,
                                                         char **out_json) {
  if (!context || !out_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  if (!ctx->config_manager) {
    return VXCORE_ERR_NOT_INITIALIZED;
  }

  try {
    nlohmann::json json = ctx->config_manager->GetSessionConfig().ToJson();
    std::string json_str = json.dump(2);
    *out_json = vxcore_strdup(json_str.c_str());
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_SERIALIZE;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API void vxcore_string_free(char *str) {
  if (str) {
    free(str);
  }
}

VXCORE_API VxCoreError vxcore_log_set_level(VxCoreLogLevel level) {
  vxcore::LogLevel cpp_level;
  switch (level) {
    case VXCORE_LOG_LEVEL_TRACE:
      cpp_level = vxcore::LogLevel::kTrace;
      break;
    case VXCORE_LOG_LEVEL_DEBUG:
      cpp_level = vxcore::LogLevel::kDebug;
      break;
    case VXCORE_LOG_LEVEL_INFO:
      cpp_level = vxcore::LogLevel::kInfo;
      break;
    case VXCORE_LOG_LEVEL_WARN:
      cpp_level = vxcore::LogLevel::kWarn;
      break;
    case VXCORE_LOG_LEVEL_ERROR:
      cpp_level = vxcore::LogLevel::kError;
      break;
    case VXCORE_LOG_LEVEL_FATAL:
      cpp_level = vxcore::LogLevel::kFatal;
      break;
    case VXCORE_LOG_LEVEL_OFF:
      cpp_level = vxcore::LogLevel::kOff;
      break;
    default:
      return VXCORE_ERR_INVALID_PARAM;
  }
  vxcore::Logger::GetInstance().SetLevel(cpp_level);
  return VXCORE_OK;
}

VXCORE_API VxCoreError vxcore_log_set_file(const char *path) {
  if (!path) {
    return VXCORE_ERR_NULL_POINTER;
  }
  try {
    vxcore::Logger::GetInstance().SetLogFile(path);
    return VXCORE_OK;
  } catch (...) {
    return VXCORE_ERR_IO;
  }
}

VXCORE_API VxCoreError vxcore_log_enable_console(int enable) {
  vxcore::Logger::GetInstance().EnableConsole(enable != 0);
  return VXCORE_OK;
}
