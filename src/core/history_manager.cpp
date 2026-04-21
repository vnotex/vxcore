#include "core/history_manager.h"

#include <algorithm>

#include "core/metadata_store.h"
#include "utils/logger.h"
#include "utils/utils.h"

namespace vxcore {

HistoryEntry HistoryEntry::FromJson(const nlohmann::json &j) {
  HistoryEntry entry;
  entry.file_id = j.value("fileId", "");
  entry.opened_utc = j.value("openedUtc", static_cast<int64_t>(0));
  return entry;
}

nlohmann::json HistoryEntry::ToJson() const {
  return nlohmann::json{{"fileId", file_id}, {"openedUtc", opened_utc}};
}

std::vector<HistoryEntry> GetHistory(MetadataStore *store) {
  if (!store) {
    return {};
  }

  auto val = store->GetNotebookMetadata(kMetaKeyHistory);
  if (!val.has_value()) {
    return {};
  }

  try {
    auto arr = nlohmann::json::parse(val.value());
    if (!arr.is_array()) {
      return {};
    }
    std::vector<HistoryEntry> result;
    for (const auto &item : arr) {
      if (item.is_object()) {
        result.push_back(HistoryEntry::FromJson(item));
      }
    }
    return result;
  } catch (const nlohmann::json::exception &) {
    VXCORE_LOG_WARN("Failed to parse history JSON");
    return {};
  }
}

bool RecordFileOpen(MetadataStore *store, const std::string &file_id) {
  if (!store || file_id.empty()) {
    return false;
  }

  auto history = GetHistory(store);

  // Remove existing entry with same file_id (dedup)
  history.erase(std::remove_if(history.begin(), history.end(),
                               [&file_id](const HistoryEntry &e) { return e.file_id == file_id; }),
                history.end());

  // Prepend new entry
  HistoryEntry entry;
  entry.file_id = file_id;
  entry.opened_utc = GetCurrentTimestampMillis();
  history.insert(history.begin(), entry);

  // Truncate to max
  if (static_cast<int>(history.size()) > kMaxHistoryItems) {
    history.resize(kMaxHistoryItems);
  }

  // Serialize and store
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &e : history) {
    arr.push_back(e.ToJson());
  }
  return store->SetNotebookMetadata(kMetaKeyHistory, arr.dump());
}

bool ClearHistory(MetadataStore *store) {
  if (!store) {
    return false;
  }
  return store->SetNotebookMetadata(kMetaKeyHistory, "[]");
}

}  // namespace vxcore
