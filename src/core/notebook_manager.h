#ifndef VXCORE_NOTEBOOK_MANAGER_H
#define VXCORE_NOTEBOOK_MANAGER_H

#include "vxcore/vxcore_types.h"
#include "notebook.h"
#include "vxcore_session_config.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vxcore {

class NotebookManager {
public:
  NotebookManager(const std::string &localDataFolder, VxCoreSessionConfig *sessionConfig);
  ~NotebookManager();

  VxCoreError createNotebook(const std::string &rootFolder, NotebookType type,
                             const std::string &propertiesJson, std::string &outNotebookId);

  VxCoreError openNotebook(const std::string &rootFolder, std::string &outNotebookId);

  VxCoreError closeNotebook(const std::string &notebookId);

  VxCoreError getNotebookProperties(const std::string &notebookId, std::string &outPropertiesJson);

  VxCoreError setNotebookProperties(const std::string &notebookId,
                                    const std::string &propertiesJson);

  VxCoreError listNotebooks(std::string &outNotebooksJson);

  Notebook *getNotebook(const std::string &notebookId);

  void setSessionConfigUpdater(std::function<void()> updater);

private:
  VxCoreError loadNotebookRecord(const std::string &rootFolder, NotebookRecord &record);
  VxCoreError saveNotebookRecord(const NotebookRecord &record);
  NotebookRecord *findNotebookRecord(const std::string &id);
  void updateSessionConfig();

  std::string localDataFolder_;
  VxCoreSessionConfig *sessionConfig_;
  std::map<std::string, std::unique_ptr<Notebook>> notebooks_;
  std::mutex mutex_;
  std::function<void()> sessionConfigUpdater_;
};

} // namespace vxcore

#endif
