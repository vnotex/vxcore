#ifndef VXCORE_BUNDLED_NOTEBOOK_H_
#define VXCORE_BUNDLED_NOTEBOOK_H_

#include "notebook.h"

namespace vxcore {

class BundledNotebook : public Notebook {
 public:
  static VxCoreError Create(const std::string &root_folder, const NotebookConfig *overridden_config,
                            std::unique_ptr<Notebook> &out_notebook);

  static VxCoreError Open(const std::string &root_folder, std::unique_ptr<Notebook> &out_notebook);

  VxCoreError UpdateConfig(const NotebookConfig &config) override;

 private:
  BundledNotebook(const std::string &root_folder);

  VxCoreError LoadConfig();
  VxCoreError InitOnCreation();
};

}  // namespace vxcore

#endif  // VXCORE_BUNDLED_NOTEBOOK_H_
