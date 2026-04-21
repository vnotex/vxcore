#ifndef VXCORE_HISTORY_MANAGER_H
#define VXCORE_HISTORY_MANAGER_H

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace vxcore {

constexpr int kMaxHistoryItems = 50;
constexpr const char *kMetaKeyHistory = "history";

struct HistoryEntry {
  std::string file_id;
  int64_t opened_utc;

  static HistoryEntry FromJson(const nlohmann::json &j);
  nlohmann::json ToJson() const;
};

class MetadataStore;

std::vector<HistoryEntry> GetHistory(MetadataStore *store);
bool RecordFileOpen(MetadataStore *store, const std::string &file_id);
bool ClearHistory(MetadataStore *store);

}  // namespace vxcore

#endif  // VXCORE_HISTORY_MANAGER_H
