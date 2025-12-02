#ifndef VXCORE_RAW_NOTEBOOK_H_
#define VXCORE_RAW_NOTEBOOK_H_

#include "notebook.h"

namespace vxcore {

class RawNotebook : public Notebook {
 public:
  static VxCoreError Create(const std::string &root_folder, const NotebookConfig &config,
                            std::unique_ptr<Notebook> &out_notebook);

  VxCoreError UpdateConfig(const NotebookConfig &config) override;

 private:
  RawNotebook(const std::string &root_folder);
};

}  // namespace vxcore

#endif  // VXCORE_RAW_NOTEBOOK_H_
