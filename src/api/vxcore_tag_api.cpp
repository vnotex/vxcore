#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <utility>

#include "api/api_utils.h"
#include "core/context.h"
#include "core/metadata_store.h"
#include "core/notebook_manager.h"
#include "vxcore/vxcore.h"

namespace {

VxCoreUtf8View Utf8View(const std::string &text) { return {text.data(), text.size()}; }

VxCoreError FindFilesByTags(vxcore::VxCoreContext &ctx, vxcore::Notebook &notebook,
                            const char *tags_json, const char *op,
                            std::vector<vxcore::StoreTagQueryResult> &results) {
  nlohmann::json tags_array;
  try {
    tags_array = nlohmann::json::parse(tags_json);
  } catch (...) {
    ctx.last_error = "Invalid tags JSON";
    return VXCORE_ERR_JSON_PARSE;
  }
  if (!tags_array.is_array()) {
    ctx.last_error = "tags_json must be a JSON array";
    return VXCORE_ERR_INVALID_PARAM;
  }

  std::vector<std::string> tags;
  tags.reserve(tags_array.size());
  for (const auto &tag : tags_array) {
    if (!tag.is_string()) {
      ctx.last_error = "Each tag must be a string";
      return VXCORE_ERR_INVALID_PARAM;
    }
    tags.push_back(tag.get<std::string>());
  }

  const std::string op_str(op);
  bool use_and;
  if (op_str == "AND") {
    use_and = true;
  } else if (op_str == "OR") {
    use_and = false;
  } else {
    ctx.last_error = "Operator must be \"AND\" or \"OR\"";
    return VXCORE_ERR_INVALID_PARAM;
  }

  const auto error = notebook.FindFilesByTags(tags, use_and, results);
  if (error != VXCORE_OK) {
    ctx.last_error = "Failed to find files by tags";
  }
  return error;
}

void SetTagError(vxcore::VxCoreContext &ctx, const char *message) noexcept {
  try {
    ctx.last_error = message;
  } catch (...) {
    // Error reporting must not let an allocation exception cross the C ABI.
  }
}

}  // namespace

struct VxCoreTagList {
  explicit VxCoreTagList(const std::vector<vxcore::TagNode> &tags) : entries_(tags.size()) {
    size_t identity_bytes = 0;
    for (const auto &tag : tags) {
      identity_bytes += tag.name.size() + tag.parent.size();
    }
    bytes_.reserve(identity_bytes);
    for (size_t i = 0; i < tags.size(); ++i) {
      const auto &tag = tags[i];
      auto &entry = entries_[i];
      entry.name.size = tag.name.size();
      bytes_ += tag.name;
      entry.parent.size = tag.parent.size();
      bytes_ += tag.parent;
      if (!tag.metadata.is_object() || !tag.metadata.empty()) {
        const auto begin = bytes_.size();
        bytes_ += tag.metadata.dump();
        entry.metadata_json.size = bytes_.size() - begin;
      }
    }
    // Publish only after all appends; no live config pointers or per-field owners.
    size_t offset = 0;
    for (auto &entry : entries_) {
      for (auto *text : {&entry.name, &entry.parent, &entry.metadata_json}) {
        text->data = bytes_.data() + offset;
        offset += text->size;
      }
    }
    view_ = {entries_.data(), entries_.size()};
  }

  std::string bytes_;
  std::vector<VxCoreTagEntryView> entries_;
  VxCoreTagListView view_{};
};

struct VxCoreTagFileResult {
  explicit VxCoreTagFileResult(std::vector<vxcore::StoreTagQueryResult> &&files)
      : files_(std::move(files)), entries_(files_.size()) {
    size_t tag_count = 0;
    for (const auto &file : files_) {
      tag_count += file.tags.size();
    }
    tags_.reserve(tag_count);
    for (size_t i = 0; i < files_.size(); ++i) {
      const auto &file = files_[i];
      const auto offset = tags_.size();
      for (const auto &tag : file.tags) {
        tags_.push_back(Utf8View(tag));
      }
      entries_[i] = {Utf8View(file.file_path), Utf8View(file.file_name),
                     file.tags.empty() ? nullptr : tags_.data() + offset, file.tags.size()};
    }
    view_ = {entries_.data(), entries_.size()};
  }

  std::vector<vxcore::StoreTagQueryResult> files_;
  std::vector<VxCoreUtf8View> tags_;
  std::vector<VxCoreTagFileEntryView> entries_;
  VxCoreTagFileResultView view_{};
};

struct VxCoreTagCounts {
  explicit VxCoreTagCounts(std::vector<std::pair<std::string, int>> &&counts)
      : counts_(std::move(counts)), entries_(counts_.size()) {
    for (size_t i = 0; i < counts_.size(); ++i) {
      entries_[i] = {Utf8View(counts_[i].first), counts_[i].second};
    }
    view_ = {entries_.data(), entries_.size()};
  }

  std::vector<std::pair<std::string, int>> counts_;
  std::vector<VxCoreTagCountEntryView> entries_;
  VxCoreTagCountsView view_{};
};

VXCORE_API const VxCoreTagListView *vxcore_tag_list_view(const VxCoreTagList *result) {
  return result ? &result->view_ : nullptr;
}

VXCORE_API void vxcore_tag_list_free(VxCoreTagList *result) { delete result; }

VXCORE_API const VxCoreTagFileResultView *vxcore_tag_file_result_view(
    const VxCoreTagFileResult *result) {
  return result ? &result->view_ : nullptr;
}

VXCORE_API void vxcore_tag_file_result_free(VxCoreTagFileResult *result) { delete result; }

VXCORE_API const VxCoreTagCountsView *vxcore_tag_counts_view(const VxCoreTagCounts *result) {
  return result ? &result->view_ : nullptr;
}

VXCORE_API void vxcore_tag_counts_free(VxCoreTagCounts *result) { delete result; }

VXCORE_API VxCoreError vxcore_tag_create(VxCoreContextHandle context, const char *notebook_id,
                                         const char *tag_name) {
  if (!context || !notebook_id || !tag_name) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    VxCoreError err = notebook->CreateTag(tag_name, "");
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to create tag";
      return err;
    }

    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error creating tag";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_create_path(VxCoreContextHandle context, const char *notebook_id,
                                              const char *tag_path) {
  if (!context || !notebook_id || !tag_path) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    VxCoreError err = notebook->CreateTagPath(tag_path);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to create tag path";
      return err;
    }

    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error creating tag path";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_delete(VxCoreContextHandle context, const char *notebook_id,
                                         const char *tag_name) {
  if (!context || !notebook_id || !tag_name) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    VxCoreError err = notebook->DeleteTag(tag_name);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to delete tag";
      return err;
    }

    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error deleting tag";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_list(VxCoreContextHandle context, const char *notebook_id,
                                       char **out_tags_json) {
  if (!context || !notebook_id || !out_tags_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    const auto &tags = notebook->GetTags();

    nlohmann::json tags_json = nlohmann::json::array();
    for (const auto &tag : tags) {
      tags_json.push_back(tag.ToJson());
    }
    char *json_copy = vxcore_strdup(tags_json.dump().c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_tags_json = json_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error listing tags";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_move(VxCoreContextHandle context, const char *notebook_id,
                                       const char *tag_name, const char *parent_tag) {
  if (!context || !notebook_id || !tag_name || !parent_tag) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    VxCoreError err = notebook->MoveTag(tag_name, parent_tag);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to move tag";
      return err;
    }

    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error moving tag";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_find_files(VxCoreContextHandle context, const char *notebook_id,
                                             const char *tags_json, const char *op,
                                             char **out_results_json) {
  if (!context || !notebook_id || !tags_json || !op || !out_results_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    std::vector<vxcore::StoreTagQueryResult> results;
    const auto err = FindFilesByTags(*ctx, *notebook, tags_json, op, results);
    if (err != VXCORE_OK) {
      return err;
    }

    nlohmann::json matches = nlohmann::json::array();
    for (const auto &result : results) {
      matches.push_back(
          {{"filePath", result.file_path}, {"fileName", result.file_name}, {"tags", result.tags}});
    }
    nlohmann::json output = {{"matchCount", static_cast<int>(matches.size())},
                             {"matches", std::move(matches)}};
    char *json_copy = vxcore_strdup(output.dump().c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_results_json = json_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error finding files by tags";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_count_files_by_tag(VxCoreContextHandle context,
                                                     const char *notebook_id,
                                                     char **out_results_json) {
  if (!context || !notebook_id || !out_results_json) {
    return VXCORE_ERR_NULL_POINTER;
  }

  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);

  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }

    std::vector<std::pair<std::string, int>> counts;
    VxCoreError err = notebook->CountFilesByTag(counts);
    if (err != VXCORE_OK) {
      ctx->last_error = "Failed to count files by tag";
      return err;
    }

    nlohmann::json results = nlohmann::json::array();
    for (const auto &count : counts) {
      results.push_back({{"tag", count.first}, {"count", count.second}});
    }
    char *json_copy = vxcore_strdup(results.dump().c_str());
    if (!json_copy) {
      return VXCORE_ERR_OUT_OF_MEMORY;
    }

    *out_results_json = json_copy;
    return VXCORE_OK;
  } catch (...) {
    ctx->last_error = "Unknown error counting files by tag";
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_list_typed(VxCoreContextHandle context, const char *notebook_id,
                                             VxCoreTagList **out_result) {
  if (out_result) {
    *out_result = nullptr;
  }
  if (!context || !notebook_id || !out_result) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }
    auto result = std::make_unique<VxCoreTagList>(notebook->GetTags());
    *out_result = result.release();
    return VXCORE_OK;
  } catch (const std::bad_alloc &) {
    SetTagError(*ctx, "Out of memory listing tags");
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (...) {
    SetTagError(*ctx, "Unknown error listing tags");
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_find_files_typed(VxCoreContextHandle context,
                                                   const char *notebook_id, const char *tags_json,
                                                   const char *op,
                                                   VxCoreTagFileResult **out_result) {
  if (out_result) {
    *out_result = nullptr;
  }
  if (!context || !notebook_id || !tags_json || !op || !out_result) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }
    std::vector<vxcore::StoreTagQueryResult> files;
    const auto error = FindFilesByTags(*ctx, *notebook, tags_json, op, files);
    if (error != VXCORE_OK) {
      return error;
    }
    auto result = std::make_unique<VxCoreTagFileResult>(std::move(files));
    *out_result = result.release();
    return VXCORE_OK;
  } catch (const std::bad_alloc &) {
    SetTagError(*ctx, "Out of memory finding files by tags");
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (...) {
    SetTagError(*ctx, "Unknown error finding files by tags");
    return VXCORE_ERR_UNKNOWN;
  }
}

VXCORE_API VxCoreError vxcore_tag_count_files_by_tag_typed(VxCoreContextHandle context,
                                                           const char *notebook_id,
                                                           VxCoreTagCounts **out_result) {
  if (out_result) {
    *out_result = nullptr;
  }
  if (!context || !notebook_id || !out_result) {
    return VXCORE_ERR_NULL_POINTER;
  }
  auto *ctx = reinterpret_cast<vxcore::VxCoreContext *>(context);
  try {
    auto notebook = ctx->notebook_manager->GetNotebook(notebook_id);
    if (!notebook) {
      ctx->last_error = "Notebook not found";
      return VXCORE_ERR_NOT_FOUND;
    }
    std::vector<std::pair<std::string, int>> counts;
    const auto error = notebook->CountFilesByTag(counts);
    if (error != VXCORE_OK) {
      ctx->last_error = "Failed to count files by tag";
      return error;
    }
    auto result = std::make_unique<VxCoreTagCounts>(std::move(counts));
    *out_result = result.release();
    return VXCORE_OK;
  } catch (const std::bad_alloc &) {
    SetTagError(*ctx, "Out of memory counting files by tag");
    return VXCORE_ERR_OUT_OF_MEMORY;
  } catch (...) {
    SetTagError(*ctx, "Unknown error counting files by tag");
    return VXCORE_ERR_UNKNOWN;
  }
}
