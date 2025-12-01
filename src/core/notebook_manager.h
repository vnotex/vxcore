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
  NotebookManager(VxCoreSessionConfig *session_config);
  ~NotebookManager();

  VxCoreError CreateNotebook(const std::string &root_folder, NotebookType type,
                             const std::string &config_json, std::string &out_notebook_id);

  VxCoreError OpenNotebook(const std::string &root_folder, std::string &out_notebook_id);

  VxCoreError CloseNotebook(const std::string &notebook_id);

  VxCoreError GetNotebookConfig(const std::string &notebook_id, std::string &out_config_json);

  VxCoreError UpdateNotebookConfig(const std::string &notebook_id, const std::string &config_json);

  VxCoreError ListNotebooks(std::string &out_notebooks_json);

  Notebook *GetNotebook(const std::string &notebook_id);

  void SetSessionConfigUpdater(std::function<void()> updater);

 private:
  void LoadOpenNotebooks();
  Notebook *FindNotebookByRootFolder(const std::string &root_folder);

  NotebookRecord *FindNotebookRecord(const std::string &id);
  VxCoreError UpdateNotebookRecord(const Notebook &notebook);

  nlohmann::json ToNotebookConfig(const Notebook &notebook) const;

  VxCoreSessionConfig *session_config_;
  std::map<std::string, std::unique_ptr<Notebook>> notebooks_;
  std::function<void()> session_config_updater_;
};

}  // namespace vxcore

#endif
