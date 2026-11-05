#include "vxcore/mobile/vxcore_mobile.h"

#include <new>
#include <stdexcept>
#include <utility>

#include "core/config_manager.h"
#include "core/context.h"

VXCORE_API VxCoreError vxcore_context_create_with_paths(
    const char *config_json, const char *app_data_path, const char *local_data_path,
    VxCoreContextHandle *out_context) {
  (void)config_json;
  if (!out_context) return VXCORE_ERR_NULL_POINTER;
  *out_context = nullptr;
  try {
    std::filesystem::path app_path;
    std::filesystem::path local_path;
    const auto err = vxcore::ConfigManager::ValidateDataPaths(
        app_data_path, local_data_path, app_path, local_path);
    if (err != VXCORE_OK) return err;
    return vxcore::InitializeContext(std::make_unique<vxcore::ConfigManager>(
                                 std::move(app_path), std::move(local_path)), out_context);
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::length_error &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const nlohmann::json::exception &) {
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::filesystem::filesystem_error &) {
    return VXCORE_ERR_IO;
  } catch (...) {
    return VXCORE_ERR_UNKNOWN;
  }
}
