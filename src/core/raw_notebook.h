#ifndef VXCORE_RAW_NOTEBOOK_H_
#define VXCORE_RAW_NOTEBOOK_H_

#include "notebook.h"

namespace vxcore {

class RawNotebook : public Notebook {
 public:
  static VxCoreError Create(const std::string &local_data_folder, const std::string &root_folder,
                            const NotebookConfig *overridden_config,
                            std::unique_ptr<Notebook> &out_notebook);

  static VxCoreError Open(const std::string &local_data_folder, const std::string &root_folder,
                          const std::string &id, std::unique_ptr<Notebook> &out_notebook);

  VxCoreError UpdateConfig(const NotebookConfig &config) override;
  VxCoreError RebuildCache() override;

  std::string GetRecycleBinPath() const override;
  VxCoreError EmptyRecycleBin() override;

  // Tag operations are unsupported for raw notebooks
  VxCoreError CreateTag(const std::string &tag_name,
                        const std::string &parent_tag = "") override;
  VxCoreError CreateTagPath(const std::string &tag_path) override;
  VxCoreError DeleteTag(const std::string &tag_name) override;
  VxCoreError MoveTag(const std::string &tag_name, const std::string &parent_tag) override;
  VxCoreError GetTags(std::string &out_tags_json) const override;
  VxCoreError FindFilesByTags(const std::vector<std::string> &tags, bool use_and,
                              std::string &out_results_json) override;
  VxCoreError CountFilesByTag(std::string &out_results_json) override;

 private:
  RawNotebook(const std::string &local_data_folder, const std::string &root_folder);

  std::string GetMetadataFolder() const override;
  std::string GetConfigPath() const override;

  VxCoreError LoadConfig();
  VxCoreError SaveConfigToDb();
  VxCoreError InitOnCreation();
};

}  // namespace vxcore

#endif  // VXCORE_RAW_NOTEBOOK_H_
