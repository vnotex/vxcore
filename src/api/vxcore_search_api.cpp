#include <memory>
#include <new>

#include "api/api_utils.h"
#include "core/config_manager.h"
#include "core/context.h"
#include "core/notebook_manager.h"
#include "core/work_queue.h"
#include "search/search_manager.h"
#include "search/search_queue_name.h"
#include "vxcore/notebook_json_keys.h"
#include "vxcore/vxcore.h"

namespace {

VxCoreUtf8View Utf8View(const std::string &text) { return {text.data(), text.size()}; }

// Only the flat view arrays are allocated. All text continues to belong to native
// result/chunk storage, which must be finalized before constructing this projection.
struct ContentSearchProjection {
  std::vector<VxCoreContentSearchFileView> files;
  std::vector<VxCoreSearchOccurrenceView> matches;
  VxCoreContentSearchView view{};

  ContentSearchProjection(const std::vector<vxcore::ContentSearchMatchedFile> &native_files,
                          bool truncated, size_t encrypted_skipped) {
    size_t match_count = 0;
    for (const auto &file : native_files) {
      match_count += file.matches.size();
    }
    files.reserve(native_files.size());
    matches.reserve(match_count);
    for (const auto &file : native_files) {
      const size_t offset = matches.size();
      for (const auto &match : file.matches) {
        matches.push_back(
            {match.line_number, match.column_start, match.column_end, Utf8View(match.line_text)});
      }
      files.push_back({Utf8View(file.path), Utf8View(file.id),
                       file.matches.empty() ? nullptr : matches.data() + offset,
                       file.matches.size()});
    }
    view = {files.data(), files.size(), encrypted_skipped, static_cast<uint8_t>(truncated)};
  }
};

std::string SerializeFileResults(const vxcore::FileSearchResult &native) {
  nlohmann::json result;
  result["matchCount"] = native.files.size();
  result["truncated"] = native.truncated;
  result["matches"] = nlohmann::json::array();
  for (const auto &file : native.files) {
    result["matches"].push_back(file.ToJson());
  }
  return result.dump();
}

std::string SerializeContentResults(const std::vector<vxcore::ContentSearchMatchedFile> &files,
                                    bool truncated, size_t encrypted_skipped) {
  nlohmann::json result;
  result["matchCount"] = files.size();
  result["truncated"] = truncated;
  result["matches"] = nlohmann::json::array();
  if (encrypted_skipped != 0) {
    result[vxcore::kJsonKeyEncryptedSkippedCount] = encrypted_skipped;
  }
  for (const auto &file : files) {
    nlohmann::json item;
    item["path"] = file.path;
    item["id"] = file.id;
    item["matchCount"] = file.matches.size();
    item["matches"] = nlohmann::json::array();
    for (const auto &match : file.matches) {
      item["matches"].push_back({{"lineNumber", match.line_number},
                                 {"columnStart", match.column_start},
                                 {"columnEnd", match.column_end},
                                 {"lineText", match.line_text}});
    }
    result["matches"].push_back(std::move(item));
  }
  return result.dump();
}

// Error reporting must itself remain inside the C exception boundary.
void SetSearchError(vxcore::VxCoreContext *ctx, const char *message) noexcept {
  try {
    ctx->last_error = message;
  } catch (...) {
  }
}

}  // namespace

struct VxCoreFileSearchResult {
  vxcore::FileSearchResult native;
  std::vector<VxCoreFileSearchEntryView> files;
  std::vector<VxCoreUtf8View> tags;
  VxCoreFileSearchView view{};

  explicit VxCoreFileSearchResult(vxcore::FileSearchResult &&result) : native(std::move(result)) {
    size_t tag_count = 0;
    for (const auto &file : native.files) {
      tag_count += file.tags.size();
    }
    files.reserve(native.files.size());
    tags.reserve(tag_count);
    for (const auto &file : native.files) {
      const size_t offset = tags.size();
      for (const auto &tag : file.tags) {
        tags.push_back(Utf8View(tag));
      }
      files.push_back({Utf8View(file.path), Utf8View(file.absolute_path), Utf8View(file.id),
                       file.created_utc, file.modified_utc,
                       file.tags.empty() ? nullptr : tags.data() + offset, file.tags.size(),
                       static_cast<uint8_t>(file.is_folder)});
    }
    view = {files.data(), files.size(), static_cast<uint8_t>(native.truncated)};
  }
};

struct VxCoreContentSearchResult {
  vxcore::ManagedContentSearchResult native;
  ContentSearchProjection projection;

  explicit VxCoreContentSearchResult(vxcore::ManagedContentSearchResult &&result)
      : native(std::move(result)),
        projection(native.content.matched_files, native.content.truncated,
                   native.encrypted_skipped_count) {}
};

VXCORE_API const VxCoreFileSearchView *vxcore_file_search_result_view(
    const VxCoreFileSearchResult *result) {
  return result ? &result->view : nullptr;
}

VXCORE_API void vxcore_file_search_result_free(VxCoreFileSearchResult *result) { delete result; }

VXCORE_API const VxCoreContentSearchView *vxcore_content_search_result_view(
    const VxCoreContentSearchResult *result) {
  return result ? &result->projection.view : nullptr;
}

VXCORE_API void vxcore_content_search_result_free(VxCoreContentSearchResult *result) {
  delete result;
}

std::unique_ptr<vxcore::SearchManager> CreateSearchManager(vxcore::VxCoreContext *ctx,
                                                           vxcore::Notebook *notebook,
                                                           vxcore::WorkQueue *queue,
                                                           const volatile int *cancel_flag) {
  const vxcore::FileTypesConfig *file_types = nullptr;
  std::vector<std::string> encodings;
  if (ctx && ctx->config_manager) {
    const auto &config = ctx->config_manager->GetConfig();
    file_types = &config.file_types;
    encodings = config.search.encodings;
  }
  auto manager =
      std::make_unique<vxcore::SearchManager>(notebook, file_types, std::move(encodings));
  manager->SetWorkQueue(queue);
  manager->SetCancelFlag(cancel_flag);
  return manager;
}

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

    auto search_manager = CreateSearchManager(ctx, notebook, nullptr, nullptr);

    vxcore::FileSearchResult result;
    std::string input_files_str = input_files_json ? input_files_json : "";
    VxCoreError err = search_manager->SearchFiles(query_json, input_files_str, result);
    if (err != VXCORE_OK) {
      ctx->last_error = "Search files failed";
      return err;
    }

    char *json_copy = vxcore_strdup(SerializeFileResults(result).c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_results_json = json_copy;
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    SetSearchError(ctx, "Search files failed");
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::exception &e) {
    SetSearchError(ctx, e.what());
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    SetSearchError(ctx, "Unknown error searching files");
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

    auto search_manager = CreateSearchManager(
        ctx, notebook, ctx->work_queue_manager->GetOrCreate(vxcore::kSearchQueueName), nullptr);

    vxcore::ManagedContentSearchResult result;
    std::string input_files_str = input_files_json ? input_files_json : "";
    VxCoreError err = search_manager->SearchContent(query_json, input_files_str, result);
    if (err != VXCORE_OK) {
      ctx->last_error = "Search content failed";
      return err;
    }

    char *json_copy = vxcore_strdup(SerializeContentResults(result.content.matched_files,
                                                            result.content.truncated,
                                                            result.encrypted_skipped_count)
                                        .c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_results_json = json_copy;
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    SetSearchError(ctx, "Search content failed");
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::exception &e) {
    SetSearchError(ctx, e.what());
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    SetSearchError(ctx, "Unknown error searching content");
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_search_content_ex(VxCoreContextHandle context,
                                                const char *notebook_id, const char *query_json,
                                                const char *input_files_json,
                                                volatile int *cancel_flag,
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

    auto search_manager = CreateSearchManager(
        ctx, notebook, ctx->work_queue_manager->GetOrCreate(vxcore::kSearchQueueName), cancel_flag);

    vxcore::ManagedContentSearchResult result;
    std::string input_files_str = input_files_json ? input_files_json : "";
    VxCoreError err = search_manager->SearchContent(query_json, input_files_str, result);
    if (err != VXCORE_OK) {
      if (err == VXCORE_ERR_CANCELLED) {
        return err;  // Don't overwrite with generic error message
      }
      ctx->last_error = "Search content failed";
      return err;
    }

    char *json_copy = vxcore_strdup(SerializeContentResults(result.content.matched_files,
                                                            result.content.truncated,
                                                            result.encrypted_skipped_count)
                                        .c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_results_json = json_copy;
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    SetSearchError(ctx, "Search content failed");
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::exception &e) {
    SetSearchError(ctx, e.what());
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    SetSearchError(ctx, "Unknown error searching content");
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_search_content_streaming(VxCoreContextHandle context,
                                                       const char *notebook_id,
                                                       const char *query_json,
                                                       const char *input_files_json, int batch_size,
                                                       VxCoreSearchBatchCallback batch_cb,
                                                       void *userdata, volatile int *cancel_flag) {
  if (!context || !notebook_id || !query_json || !batch_cb) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    auto search_manager = CreateSearchManager(
        ctx, notebook, ctx->work_queue_manager->GetOrCreate(vxcore::kSearchQueueName), cancel_flag);

    std::string input_files_str = input_files_json ? input_files_json : "";

    auto on_batch = [batch_cb, userdata](int batch_index, int total_batches,
                                         const std::vector<vxcore::ContentSearchMatchedFile> &files,
                                         size_t encrypted_skipped) {
      const auto json = SerializeContentResults(files, false, encrypted_skipped);
      batch_cb(batch_index, total_batches, json.c_str(), userdata);
    };

    VxCoreError err =
        search_manager->SearchContentStreaming(query_json, input_files_str, batch_size, on_batch);
    if (err != VXCORE_OK && err != VXCORE_ERR_CANCELLED) {
      ctx->last_error = "Search content streaming failed";
    }
    return err;
  } catch (const std::exception &e) {
    SetSearchError(ctx, e.what());
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    SetSearchError(ctx, "Unknown error streaming content search");
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

    vxcore::FileSearchResult result;
    std::string input_files_str = input_files_json ? input_files_json : "";
    VxCoreError err = search_manager->SearchByTags(query_json, input_files_str, result);
    if (err != VXCORE_OK) {
      ctx->last_error = "Search by tags failed";
      return err;
    }

    char *json_copy = vxcore_strdup(SerializeFileResults(result).c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_results_json = json_copy;
    return VXCORE_OK;
  } catch (const nlohmann::json::exception &) {
    SetSearchError(ctx, "Search by tags failed");
    return VXCORE_ERR_JSON_PARSE;
  } catch (const std::exception &e) {
    SetSearchError(ctx, e.what());
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    SetSearchError(ctx, "Unknown error searching by tags");
    return VXCORE_ERR_UNKNOWN;
  }
}

namespace {

VxCoreError SearchFilesTyped(VxCoreContextHandle context, const char *notebook_id,
                             const char *query_json, const char *input_files_json,
                             VxCoreFileSearchResult **out_result, bool by_tags) {
  if (out_result) {
    *out_result = nullptr;
  }
  if (!context || !notebook_id || !query_json || !out_result) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }
    auto manager = by_tags ? std::make_unique<vxcore::SearchManager>(notebook)
                           : CreateSearchManager(ctx, notebook, nullptr, nullptr);
    vxcore::FileSearchResult result;
    const std::string input = input_files_json ? input_files_json : "";
    const auto err = by_tags ? manager->SearchByTags(query_json, input, result)
                             : manager->SearchFiles(query_json, input, result);
    if (err != VXCORE_OK) {
      ctx->last_error = by_tags ? "Search by tags failed" : "Search files failed";
      return err;
    }
    auto owner = std::make_unique<VxCoreFileSearchResult>(std::move(result));
    *out_result = owner.release();
    return VXCORE_OK;
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::exception &e) {
    SetSearchError(ctx, e.what());
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    SetSearchError(ctx,
                   by_tags ? "Unknown error searching by tags" : "Unknown error searching files");
    return VXCORE_ERR_UNKNOWN;
  }
}

VxCoreError SearchContentTyped(VxCoreContextHandle context, const char *notebook_id,
                               const char *query_json, const char *input_files_json,
                               volatile int *cancel_flag, VxCoreContentSearchResult **out_result) {
  if (out_result) {
    *out_result = nullptr;
  }
  if (!context || !notebook_id || !query_json || !out_result) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }
    auto manager = CreateSearchManager(
        ctx, notebook, ctx->work_queue_manager->GetOrCreate(vxcore::kSearchQueueName), cancel_flag);
    vxcore::ManagedContentSearchResult result;
    const auto err =
        manager->SearchContent(query_json, input_files_json ? input_files_json : "", result);
    if (err != VXCORE_OK) {
      if (err != VXCORE_ERR_CANCELLED) {
        ctx->last_error = "Search content failed";
      }
      return err;
    }
    auto owner = std::make_unique<VxCoreContentSearchResult>(std::move(result));
    *out_result = owner.release();
    return VXCORE_OK;
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::exception &e) {
    SetSearchError(ctx, e.what());
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    SetSearchError(ctx, "Unknown error searching content");
    return VXCORE_ERR_UNKNOWN;
  }
}

}  // namespace

VXCORE_API VxCoreError vxcore_search_files_typed(VxCoreContextHandle context,
                                                 const char *notebook_id, const char *query_json,
                                                 const char *input_files_json,
                                                 VxCoreFileSearchResult **out_result) {
  return SearchFilesTyped(context, notebook_id, query_json, input_files_json, out_result, false);
}

VXCORE_API VxCoreError vxcore_search_by_tags_typed(VxCoreContextHandle context,
                                                   const char *notebook_id, const char *query_json,
                                                   const char *input_files_json,
                                                   VxCoreFileSearchResult **out_result) {
  return SearchFilesTyped(context, notebook_id, query_json, input_files_json, out_result, true);
}

VXCORE_API VxCoreError vxcore_search_content_typed(VxCoreContextHandle context,
                                                   const char *notebook_id, const char *query_json,
                                                   const char *input_files_json,
                                                   VxCoreContentSearchResult **out_result) {
  return SearchContentTyped(context, notebook_id, query_json, input_files_json, nullptr,
                            out_result);
}

VXCORE_API VxCoreError vxcore_search_content_ex_typed(VxCoreContextHandle context,
                                                      const char *notebook_id,
                                                      const char *query_json,
                                                      const char *input_files_json,
                                                      volatile int *cancel_flag,
                                                      VxCoreContentSearchResult **out_result) {
  return SearchContentTyped(context, notebook_id, query_json, input_files_json, cancel_flag,
                            out_result);
}

VXCORE_API VxCoreError vxcore_search_content_streaming_typed(
    VxCoreContextHandle context, const char *notebook_id, const char *query_json,
    const char *input_files_json, int batch_size, VxCoreSearchBatchTypedCallback batch_cb,
    void *userdata, volatile int *cancel_flag) {
  if (!context || !notebook_id || !query_json || !batch_cb) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    auto *notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }
    auto manager = CreateSearchManager(
        ctx, notebook, ctx->work_queue_manager->GetOrCreate(vxcore::kSearchQueueName), cancel_flag);
    const auto on_batch = [batch_cb, userdata](
                              int index, int total,
                              const std::vector<vxcore::ContentSearchMatchedFile> &files,
                              size_t encrypted_skipped) {
      ContentSearchProjection projection(files, false, encrypted_skipped);
      batch_cb(index, total, &projection.view, userdata);
    };
    const auto err = manager->SearchContentStreaming(
        query_json, input_files_json ? input_files_json : "", batch_size, on_batch);
    if (err != VXCORE_OK && err != VXCORE_ERR_CANCELLED) {
      ctx->last_error = "Search content streaming failed";
    }
    return err;
  } catch (const std::bad_alloc &) {
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (const std::exception &e) {
    SetSearchError(ctx, e.what());
    return VXCORE_ERR_UNKNOWN;
  } catch (...) {
    SetSearchError(ctx, "Unknown error streaming content search");
    return VXCORE_ERR_UNKNOWN;
  }
}
