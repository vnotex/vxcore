#ifndef VXCORE_NOTEBOOK_MANAGER_H
#define VXCORE_NOTEBOOK_MANAGER_H

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "notebook.h"
#include "vxcore/vxcore_types.h"
#include "vxcore_session_config.h"

namespace vxcore {

class NotebookManager {
 public:
  NotebookManager(const std::string &local_data_folder, VxCoreSessionConfig *session_config);
  ~NotebookManager();

  VxCoreError CreateNotebook(const std::string &root_folder, NotebookType type,
                             const std::string &properties_json, std::string &out_notebook_id);

  VxCoreError OpenNotebook(const std::string &root_folder, std::string &out_notebook_id);

  VxCoreError CloseNotebook(const std::string &notebook_id);

  VxCoreError GetNotebookProperties(const std::string &notebook_id,
                                    std::string &out_properties_json);

  VxCoreError SetNotebookProperties(const std::string &notebook_id,
                                    const std::string &properties_json);

  VxCoreError ListNotebooks(std::string &out_notebooks_json);

  Notebook *GetNotebook(const std::string &notebook_id);

  void SetSessionConfigUpdater(std::function<void()> updater);

 private:
  VxCoreError LoadNotebookRecord(const std::string &root_folder, NotebookRecord &record);
  VxCoreError SaveNotebookRecord(const NotebookRecord &record);
  NotebookRecord *FindNotebookRecord(const std::string &id);
  void UpdateSessionConfig();

  std::string local_data_folder_;
  VxCoreSessionConfig *session_config_;
  std::map<std::string, std::unique_ptr<Notebook>> notebooks_;
  std::mutex mutex_;
  std::function<void()> session_config_updater_;
};

}  // namespace vxcore

#endif
