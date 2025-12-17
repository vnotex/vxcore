#include "api/api_utils.h"
#include "core/context.h"
#include "core/notebook_manager.h"
#include "search/search_manager.h"
#include "vxcore/vxcore.h"

VXCORE_API VxCoreError vxcore_search_files(VxCoreContextHandle context, const char *notebook_id,
                                           const char *query_json, const char *input_files_json,
                                           char **out_results_json) {
  if (!context || !notebook_id || !query_json || !out_results_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    auto search_manager = std::make_unique<vxcore::SearchManager>(notebook);

    std::string results_json;
    std::string input_files_str = input_files_json ? input_files_json : "";
    VxCoreError err = search_manager->SearchFiles(query_json, input_files_str, results_json);
    if (err != VXCORE_OK) {
      ctx->last_error = "Search files failed";
      return err;
    }

    char *json_copy = vxcore_strdup(results_json.c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_results_json = json_copy;
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error searching files";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_search_content(VxCoreContextHandle context, const char *notebook_id,
                                             const char *query_json, const char *input_files_json,
                                             char **out_results_json) {
  if (!context || !notebook_id || !query_json || !out_results_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    auto search_manager = std::make_unique<vxcore::SearchManager>(notebook);

    std::string results_json;
    std::string input_files_str = input_files_json ? input_files_json : "";
    VxCoreError err = search_manager->SearchContent(query_json, input_files_str, results_json);
    if (err != VXCORE_OK) {
      ctx->last_error = "Search content failed";
      return err;
    }

    char *json_copy = vxcore_strdup(results_json.c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_results_json = json_copy;
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error searching content";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_search_by_tags(VxCoreContextHandle context, const char *notebook_id,
                                             const char *query_json, const char *input_files_json,
                                             char **out_results_json) {
  if (!context || !notebook_id || !query_json || !out_results_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    auto search_manager = std::make_unique<vxcore::SearchManager>(notebook);

    std::string results_json;
    std::string input_files_str = input_files_json ? input_files_json : "";
    VxCoreError err = search_manager->SearchByTags(query_json, input_files_str, results_json);
    if (err != VXCORE_OK) {
      ctx->last_error = "Search by tags failed";
      return err;
    }

    char *json_copy = vxcore_strdup(results_json.c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_results_json = json_copy;
    return VXCORE_OK;
  } catch (const std::exception &e) {
    ctx->last_error = e.what();
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    ctx->last_error = "Unknown error searching by tags";
    return VXCORE_ERR_UNKNOWN;
  }
}
