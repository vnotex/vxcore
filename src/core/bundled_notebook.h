#ifndef VXCORE_BUNDLED_NOTEBOOK_H_
#define VXCORE_BUNDLED_NOTEBOOK_H_

#include "notebook.h"

namespace vxcore {

class BundledNotebook : public Notebook {
 public:
  // @root_folder: does not contain any notebook config.
  static VxCoreError Create(const std::string &local_data_folder, const std::string &root_folder,
                            const NotebookConfig *overridden_config,
                            std::unique_ptr<Notebook> &out_notebook);

  // @root_folder: contains existing notebook config.
  static VxCoreError Open(const std::string &local_data_folder, const std::string &root_folder,
                          std::unique_ptr<Notebook> &out_notebook);

  VxCoreError UpdateConfig(const NotebookConfig &config) override;

 private:
  BundledNotebook(const std::string &local_data_folder, const std::string &root_folder);

  std::string GetMetadataFolder() const override;
  std::string GetConfigPath() const override;

  VxCoreError LoadConfig();
  VxCoreError InitOnCreation();
};

}  // namespace vxcore

#endif  // VXCORE_BUNDLED_NOTEBOOK_H_
