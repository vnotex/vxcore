#ifndef VXCORE_FOLDER_H
#define VXCORE_FOLDER_H

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vxcore/vxcore_types.h"

namespace vxcore {

struct FileRecord {
  std::string id;
  std::string name;
  int64_t created_utc;
  int64_t modified_utc;
  nlohmann::json metadata;
  std::vector<std::string> tags;
  std::vector<std::string> attachments;  // Basenames within the note's assets directory.

  FileRecord();
  FileRecord(const std::string &name);

  // Normalize legacy relative paths without resolving them against the filesystem.
  // Invalid entries are left untouched so reading old metadata never silently drops them.
  static bool NormalizeAttachmentName(std::string &attachment, std::string_view file_id = {}) {
    if (attachment.empty() || attachment.find(':') != std::string::npos ||
        attachment.find('\0') != std::string::npos) {
      return false;
    }
    size_t start = 0;
    while (true) {
      const size_t separator = attachment.find_first_of("/\\", start);
      const size_t length =
          (separator == std::string::npos ? attachment.size() : separator) - start;
      if (length == 0) {
        return false;
      }
      if ((length == 1 && attachment[start] == '.') ||
          (length == 2 && attachment[start] == '.' && attachment[start + 1] == '.')) {
        // A configured assets folder outside the notebook produced ../.../<note-id>/name.
        // Accept that legacy prefix only for this note; never treat arbitrary traversal
        // or a trailing dot component as an attachment name.
        const size_t last_separator = attachment.find_last_of("/\\");
        if (file_id.empty() || last_separator == std::string::npos ||
            last_separator <= file_id.size()) {
          return false;
        }
        const size_t id_start = last_separator - file_id.size();
        if (id_start <= separator ||
            (attachment[id_start - 1] != '/' && attachment[id_start - 1] != '\\') ||
            attachment.compare(id_start, file_id.size(), file_id) != 0) {
          return false;
        }
      }
      if (separator == std::string::npos) {
        break;
      }
      start = separator + 1;
    }
    if (start != 0) {
      attachment.erase(0, start);
    }
    return true;
  }

  // Attachment membership is a set; preserve order while merging legacy/name duplicates.
  static bool NormalizeAttachments(std::vector<std::string> &attachments,
                                   std::string_view file_id = {}) {
    bool valid = true;
    auto end = attachments.begin();
    for (auto it = attachments.begin(); it != attachments.end(); ++it) {
      valid = NormalizeAttachmentName(*it, file_id) && valid;
      if (std::find(attachments.begin(), end, *it) == end) {
        if (end != it) {
          *end = std::move(*it);
        }
        ++end;
      }
    }
    attachments.erase(end, attachments.end());
    return valid;
  }

  static FileRecord FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
  nlohmann::json ToJsonWithType() const;

  // Metadata classifier only: OK for ordinary notes, ENCRYPTION_LOCKED for valid
  // protected notes, ENCRYPTION_FORMAT for inconsistent markers. Assets stay plaintext.
  VxCoreError CheckProtectionMetadata() const;
};

// Folder record contains all infomation about the folder itself including metadata.
struct FolderRecord {
  std::string id;
  std::string name;
  int64_t created_utc;
  int64_t modified_utc;
  nlohmann::json metadata;

  FolderRecord();
  FolderRecord(const std::string &name);
  FolderRecord(const std::string &id, const std::string &name, int64_t created_utc,
               int64_t modified_utc, const nlohmann::json &metadata);
  nlohmann::json ToJson() const;
};

struct FolderConfig {
  std::string id;
  std::string name;
  int64_t created_utc;
  int64_t modified_utc;
  nlohmann::json metadata;
  std::vector<FileRecord> files;
  std::vector<std::string> folders;

  FolderConfig();
  FolderConfig(const std::string &name);

  static FolderConfig FromJson(const nlohmann::json &json);
  nlohmann::json ToJson() const;
  nlohmann::json ToJsonWithType() const;
};

}  // namespace vxcore

#endif
