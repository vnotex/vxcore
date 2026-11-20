#ifndef VXCORE_RESULTS_H
#define VXCORE_RESULTS_H

#include "vxcore_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Immutable, in-process C ABI views. Record layouts are frozen: adding fields to
// array elements requires a new versioned type/API, not changing their stride.
// A nonzero size/count requires non-NULL data. Strings are UTF-8 byte views and
// need not be NUL-terminated. All nested pointers share their enclosing lifetime.
typedef struct {
  const char *data;
  size_t size;
} VxCoreUtf8View;

// metadata_json.size == 0 represents {}. Every other JSON value, including null,
// is encoded explicitly. Metadata is the only JSON-bearing result field.
typedef struct {
  VxCoreUtf8View id;
  VxCoreUtf8View name;
  int64_t created_utc;
  int64_t modified_utc;
  VxCoreUtf8View metadata_json;
  const VxCoreUtf8View *tags;
  size_t tag_count;
  const VxCoreUtf8View *attachments;
  size_t attachment_count;
  uint8_t exists_known;
  uint8_t exists;
} VxCoreFolderEntryView;

typedef struct {
  const VxCoreFolderEntryView *files;
  size_t file_count;
  const VxCoreFolderEntryView *folders;
  size_t folder_count;
} VxCoreFolderListingView;

typedef struct {
  VxCoreUtf8View path;
  VxCoreUtf8View absolute_path;
  VxCoreUtf8View id;
  int64_t created_utc;
  int64_t modified_utc;
  const VxCoreUtf8View *tags;
  size_t tag_count;
  uint8_t is_folder;
} VxCoreFileSearchEntryView;

typedef struct {
  const VxCoreFileSearchEntryView *files;
  size_t file_count;
  uint8_t truncated;
} VxCoreFileSearchView;

// One-based lines and zero-based, end-exclusive UTF-16 columns for every content
// search API. line_text contains UTF-8 bytes, not UTF-16 code units.
typedef struct {
  int32_t line_number;
  int32_t column_start;
  int32_t column_end;
  VxCoreUtf8View line_text;
} VxCoreSearchOccurrenceView;

typedef struct {
  VxCoreUtf8View path;
  VxCoreUtf8View id;
  const VxCoreSearchOccurrenceView *matches;
  size_t match_count;
} VxCoreContentSearchFileView;

typedef struct {
  const VxCoreContentSearchFileView *files;
  size_t file_count;
  size_t encrypted_skipped_count;
  uint8_t truncated;
} VxCoreContentSearchView;

typedef struct {
  VxCoreUtf8View name;
  VxCoreUtf8View parent;
  VxCoreUtf8View metadata_json;
} VxCoreTagEntryView;

typedef struct {
  const VxCoreTagEntryView *tags;
  size_t tag_count;
} VxCoreTagListView;

typedef struct {
  VxCoreUtf8View file_path;
  VxCoreUtf8View file_name;
  const VxCoreUtf8View *tags;
  size_t tag_count;
} VxCoreTagFileEntryView;

typedef struct {
  const VxCoreTagFileEntryView *files;
  size_t file_count;
} VxCoreTagFileResultView;

typedef struct {
  VxCoreUtf8View tag;
  int64_t count;
} VxCoreTagCountEntryView;

typedef struct {
  const VxCoreTagCountEntryView *tags;
  size_t tag_count;
} VxCoreTagCountsView;

typedef struct VxCoreFolderListing VxCoreFolderListing;
typedef struct VxCoreFileSearchResult VxCoreFileSearchResult;
typedef struct VxCoreContentSearchResult VxCoreContentSearchResult;
typedef struct VxCoreTagList VxCoreTagList;
typedef struct VxCoreTagFileResult VxCoreTagFileResult;
typedef struct VxCoreTagCounts VxCoreTagCounts;

// Each successful owning call publishes a valid result, including empty results.
// A valid out_result is reset to NULL before work and stays NULL on every error.
// The incoming *out_result is never freed; previous owners remain caller-owned.
// Views survive notebook changes/closure and context destruction until free().
// Free through the matching vxcore function, never caller free/delete. free(NULL)
// is harmless and view(NULL) returns NULL. Do not free concurrently with readers.
// Acquisition retains existing context/thread/gate restrictions; a snapshot does
// not make acquisition concurrent-safe or filesystem reads transactionally atomic.
VXCORE_API const VxCoreFolderListingView *vxcore_folder_listing_view(
    const VxCoreFolderListing *result);
VXCORE_API void vxcore_folder_listing_free(VxCoreFolderListing *result);
VXCORE_API const VxCoreFileSearchView *vxcore_file_search_result_view(
    const VxCoreFileSearchResult *result);
VXCORE_API void vxcore_file_search_result_free(VxCoreFileSearchResult *result);
VXCORE_API const VxCoreContentSearchView *vxcore_content_search_result_view(
    const VxCoreContentSearchResult *result);
VXCORE_API void vxcore_content_search_result_free(VxCoreContentSearchResult *result);
VXCORE_API const VxCoreTagListView *vxcore_tag_list_view(const VxCoreTagList *result);
VXCORE_API void vxcore_tag_list_free(VxCoreTagList *result);
VXCORE_API const VxCoreTagFileResultView *vxcore_tag_file_result_view(
    const VxCoreTagFileResult *result);
VXCORE_API void vxcore_tag_file_result_free(VxCoreTagFileResult *result);
VXCORE_API const VxCoreTagCountsView *vxcore_tag_counts_view(const VxCoreTagCounts *result);
VXCORE_API void vxcore_tag_counts_free(VxCoreTagCounts *result);

// Same inputs, ordering, and reported errors as the corresponding JSON C APIs.
// Folder arrays preserve native order. Missing bundled children remain present;
// a missing bundled parent returns VXCORE_ERR_NODE_NOT_EXISTS. Raw existence is
// unknown (exists_known == 0, exists == 1); folder tags/attachments are empty.
VXCORE_API VxCoreError vxcore_folder_list_children_typed(VxCoreContextHandle context,
                                                         const char *notebook_id,
                                                         const char *folder_path,
                                                         VxCoreFolderListing **out_result);
VXCORE_API VxCoreError vxcore_search_files_typed(VxCoreContextHandle context,
                                                 const char *notebook_id, const char *query_json,
                                                 const char *input_files_json,
                                                 VxCoreFileSearchResult **out_result);
VXCORE_API VxCoreError vxcore_search_by_tags_typed(VxCoreContextHandle context,
                                                   const char *notebook_id, const char *query_json,
                                                   const char *input_files_json,
                                                   VxCoreFileSearchResult **out_result);
VXCORE_API VxCoreError vxcore_search_content_typed(VxCoreContextHandle context,
                                                   const char *notebook_id, const char *query_json,
                                                   const char *input_files_json,
                                                   VxCoreContentSearchResult **out_result);
VXCORE_API VxCoreError vxcore_search_content_ex_typed(VxCoreContextHandle context,
                                                      const char *notebook_id,
                                                      const char *query_json,
                                                      const char *input_files_json,
                                                      volatile int *cancel_flag,
                                                      VxCoreContentSearchResult **out_result);

// Callback-only: no owned output. batch and all nested pointers are borrowed
// until return. Callbacks may overlap and arrive out of order; must not throw or
// reenter vxcore. Copy/convert before retaining data or posting a queued signal.
// Each completed chunk, even an empty one, is emitted once. A plain zero-file
// scan emits none; an all-encrypted scan emits one empty batch with skipped facts.
// encrypted_skipped_count is the scan total ONLY on batch_index == 0; otherwise 0.
// truncated is always 0. Consumers cap raw occurrences before grouping lines.
// The blocking call help-drains the existing queue and returns only after its
// work/callbacks have settled, including cancellation/error. NULL cancel disables
// cancellation; batch_size == 0 selects the default 64 files per chunk.
typedef void (*VxCoreSearchBatchTypedCallback)(int batch_index, int total_batches,
                                               const VxCoreContentSearchView *batch,
                                               void *userdata);
VXCORE_API VxCoreError vxcore_search_content_streaming_typed(
    VxCoreContextHandle context, const char *notebook_id, const char *query_json,
    const char *input_files_json, int batch_size, VxCoreSearchBatchTypedCallback batch_cb,
    void *userdata, volatile int *cancel_flag);

// Tag hierarchy remains flat: name is identity, empty parent means root. Counts
// are direct associations, not subtree sums. Raw notebooks list tags as empty
// but return VXCORE_ERR_UNSUPPORTED for tag-file queries/counts. Query inputs
// remain JSON; empty AND/OR inputs produce no matches on supported notebooks.
VXCORE_API VxCoreError vxcore_tag_list_typed(VxCoreContextHandle context, const char *notebook_id,
                                             VxCoreTagList **out_result);
VXCORE_API VxCoreError vxcore_tag_find_files_typed(VxCoreContextHandle context,
                                                   const char *notebook_id, const char *tags_json,
                                                   const char *op,
                                                   VxCoreTagFileResult **out_result);
VXCORE_API VxCoreError vxcore_tag_count_files_by_tag_typed(VxCoreContextHandle context,
                                                           const char *notebook_id,
                                                           VxCoreTagCounts **out_result);

#ifdef __cplusplus
}
#endif

#endif  // VXCORE_RESULTS_H
