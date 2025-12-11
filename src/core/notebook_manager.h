#ifndef VXCORE_NOTEBOOK_MANAGER_H
#define VXCORE_NOTEBOOK_MANAGER_H

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "notebook.h"
#include "vxcore/vxcore_types.h"

namespace vxcore {

class ConfigManager;

class NotebookManager {
 public:
  NotebookManager(ConfigManager *config_manager);
  ~NotebookManager();

  VxCoreError CreateNotebook(const std::string &root_folder, NotebookType type,
                             const std::string &config_json, std::string &out_notebook_id);

  VxCoreError OpenNotebook(const std::string &root_folder, std::string &out_notebook_id);

  VxCoreError CloseNotebook(const std::string &notebook_id);

  VxCoreError GetNotebookConfig(const std::string &notebook_id, std::string &out_config_json);

  VxCoreError UpdateNotebookConfig(const std::string &notebook_id, const std::string &config_json);

  VxCoreError ListNotebooks(std::string &out_notebooks_json);

  Notebook *GetNotebook(const std::string &notebook_id);

 private:
  void LoadOpenNotebooks();
  Notebook *FindNotebookByRootFolder(const std::string &root_folder);

  NotebookRecord *FindNotebookRecord(const std::string &id);
  VxCoreError UpdateNotebookRecord(const Notebook &notebook);

  nlohmann::json ToNotebookConfig(const Notebook &notebook) const;

  void DeleteNotebookLocalData(const Notebook &notebook);

  ConfigManager *config_manager_ = nullptr;
  std::map<std::string, std::unique_ptr<Notebook>> notebooks_;
};

}  // namespace vxcore

#endif
