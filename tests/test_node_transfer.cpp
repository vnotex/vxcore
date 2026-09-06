#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace {

const char *kCopyOptions =
    R"({"operation":"copy","conflictPolicy":"rename","timestampPolicy":"reset","createMissingTags":true,"preserveRelativeLinks":true})";
const char *kMoveOptions =
    R"({"operation":"move","conflictPolicy":"rename","timestampPolicy":"preserve","createMissingTags":true,"preserveRelativeLinks":true})";
const char *kCopyPostCommitFaultOptions =
    R"({"operation":"copy","conflictPolicy":"rename","timestampPolicy":"reset","createMissingTags":true,"preserveRelativeLinks":true,"testFault":"commitJournal"})";
const char *kMovePostCommitFaultOptions =
    R"({"operation":"move","conflictPolicy":"rename","timestampPolicy":"preserve","createMissingTags":true,"preserveRelativeLinks":true,"testFault":"postCommitVerification"})";
const char *kCopyResultBeforeCommitFaultOptions =
    R"({"operation":"copy","conflictPolicy":"rename","timestampPolicy":"reset","createMissingTags":true,"preserveRelativeLinks":true,"testFault":"resultBeforeCommit"})";
const char *kMoveResultAfterCommitFaultOptions =
    R"({"operation":"move","conflictPolicy":"rename","timestampPolicy":"preserve","createMissingTags":true,"preserveRelativeLinks":true,"testFault":"resultAfterCommit"})";
const char *kMoveSourceRemovalFaultOptions =
    R"({"operation":"move","conflictPolicy":"rename","timestampPolicy":"preserve","createMissingTags":true,"preserveRelativeLinks":true,"testFault":"sourceRemoval"})";
const char *kMoveSourceRollbackContentFaultOptions =
    R"({"operation":"move","conflictPolicy":"rename","timestampPolicy":"preserve","createMissingTags":true,"preserveRelativeLinks":true,"testFault":"sourceRollbackContent"})";
const char *kMoveSourceRollbackParentFaultOptions =
    R"({"operation":"move","conflictPolicy":"rename","timestampPolicy":"preserve","createMissingTags":true,"preserveRelativeLinks":true,"testFault":"sourceRollbackParent"})";
const char *kMoveSourceQuarantineExceptionOptions =
    R"({"operation":"move","conflictPolicy":"rename","timestampPolicy":"preserve","createMissingTags":true,"preserveRelativeLinks":true,"testFault":"sourceQuarantineException"})";
const char *kCopyDestinationRaceOptions =
    R"({"operation":"copy","conflictPolicy":"rename","timestampPolicy":"reset","createMissingTags":true,"preserveRelativeLinks":true,"testFault":"destinationPublicationRace"})";

struct Fixture {
  std::string source_path;
  std::string destination_path;
  VxCoreContextHandle context = nullptr;
  char *source_id = nullptr;
  char *destination_id = nullptr;

  explicit Fixture(const std::string &name)
      : source_path(get_test_path(name + "_source")),
        destination_path(get_test_path(name + "_destination")) {
    cleanup_test_dir(source_path);
    cleanup_test_dir(destination_path);
    if (vxcore_context_create(nullptr, &context) != VXCORE_OK) {
      return;
    }
    vxcore_notebook_create(context, source_path.c_str(), "{\"name\":\"Source\"}",
                           VXCORE_NOTEBOOK_BUNDLED, &source_id);
    vxcore_notebook_create(context, destination_path.c_str(), "{\"name\":\"Destination\"}",
                           VXCORE_NOTEBOOK_BUNDLED, &destination_id);
  }

  ~Fixture() {
    vxcore_string_free(source_id);
    vxcore_string_free(destination_id);
    vxcore_context_destroy(context);
    cleanup_test_dir(source_path);
    cleanup_test_dir(destination_path);
  }

  bool valid() const { return context && source_id && destination_id; }
};

nlohmann::json node_config(VxCoreContextHandle context, const char *notebook_id,
                           const std::string &path) {
  char *json = nullptr;
  if (vxcore_node_get_config(context, notebook_id, path.c_str(), &json) != VXCORE_OK || !json) {
    return nlohmann::json();
  }
  nlohmann::json result = nlohmann::json::parse(json);
  vxcore_string_free(json);
  return result;
}

nlohmann::json transfer(Fixture &fixture, const std::string &source_path,
                        const std::string &destination_path, const char *options,
                        VxCoreError *out_error = nullptr) {
  VxCoreNodeTransferHandle handle = nullptr;
  VxCoreError error = vxcore_node_transfer_prepare(
      fixture.context, fixture.source_id, source_path.c_str(), fixture.destination_id,
      destination_path.c_str(), options, nullptr, nullptr, &handle);
  if (error != VXCORE_OK) {
    if (out_error) {
      *out_error = error;
    }
    return nlohmann::json();
  }
  char *result_json = nullptr;
  error = vxcore_node_transfer_commit(fixture.context, handle, &result_json);
  if (out_error) {
    *out_error = error;
  }
  if (error != VXCORE_OK || !result_json) {
    return nlohmann::json();
  }
  nlohmann::json result = nlohmann::json::parse(result_json);
  vxcore_string_free(result_json);
  if (result.contains("eventBatchId")) {
    const std::string batch_id = result["eventBatchId"].get<std::string>();
    if (vxcore_node_transfer_dispatch_events(fixture.context, batch_id.c_str()) != VXCORE_OK) {
      return nlohmann::json();
    }
  }
  return result;
}

int cancel_progress(const char *, uint64_t, uint64_t, void *) { return 1; }

struct EventCapture {
  std::vector<std::string> names;
};

void capture_event(const char *name, const char *, void *userdata) {
  static_cast<EventCapture *>(userdata)->names.emplace_back(name);
}

int count_event(const EventCapture &capture, const std::string &name) {
  return static_cast<int>(std::count(capture.names.begin(), capture.names.end(), name));
}

std::string read_file_bytes(const std::string &path) {
  std::ifstream stream(utf8_to_fs_path(path), std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool filesystem_is_case_insensitive(const std::string &parent) {
  const std::string probe = parent + "/.vx-test-case-probe-a";
  const std::string alternate = parent + "/.Vx-test-case-probe-a";
  cleanup_test_dir(probe);
  create_directory(probe);
  const bool insensitive = path_exists(alternate);
  cleanup_test_dir(probe);
  return insensitive;
}

int test_file_copy_move_unicode_and_conflicts() {
  Fixture fixture("node_transfer_file");
  ASSERT(fixture.valid());
  char *folder_id = nullptr;
  ASSERT_EQ(vxcore_folder_create_path(fixture.context, fixture.destination_id, "Nested/Target",
                                      &folder_id),
            VXCORE_OK);
  vxcore_string_free(folder_id);

  char *source_file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(fixture.context, fixture.source_id, ".", "笔记.md", &source_file_id),
            VXCORE_OK);
  write_file(fixture.source_path + "/笔记.md", "# UTF-8\n内容\n");
  ASSERT_EQ(
      vxcore_node_update_timestamps(fixture.context, fixture.source_id, "笔记.md", 1001, 1002),
      VXCORE_OK);
  const nlohmann::json source_before = node_config(fixture.context, fixture.source_id, "笔记.md");

  VxCoreError error = VXCORE_ERR_UNKNOWN;
  const nlohmann::json copied = transfer(fixture, "笔记.md", "Nested/Target", kCopyOptions, &error);
  ASSERT_EQ(error, VXCORE_OK);
  ASSERT_EQ(copied["status"], "copied");
  ASSERT_EQ(copied["destinationRelativePath"], "Nested/Target/笔记.md");
  ASSERT_NE(copied["destinationNodeId"].get<std::string>(), std::string(source_file_id));
  ASSERT(path_exists(fixture.source_path + "/笔记.md"));
  ASSERT(path_exists(fixture.destination_path + "/Nested/Target/笔记.md"));
  const nlohmann::json destination_copy =
      node_config(fixture.context, fixture.destination_id, "Nested/Target/笔记.md");
  ASSERT_NE(destination_copy["createdUtc"], source_before["createdUtc"]);
  ASSERT_EQ(node_config(fixture.context, fixture.source_id, "笔记.md")["id"], source_before["id"]);

  const nlohmann::json copied_again =
      transfer(fixture, "笔记.md", "Nested/Target", kCopyOptions, &error);
  ASSERT_EQ(error, VXCORE_OK);
  ASSERT_EQ(copied_again["destinationRelativePath"], "Nested/Target/笔记 (2).md");

  char *move_file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(fixture.context, fixture.source_id, ".", "move.md", &move_file_id),
            VXCORE_OK);
  vxcore_string_free(move_file_id);
  write_file(fixture.source_path + "/move.md", "move\n");
  ASSERT_EQ(
      vxcore_node_update_timestamps(fixture.context, fixture.source_id, "move.md", 2001, 2002),
      VXCORE_OK);
  const nlohmann::json move_before = node_config(fixture.context, fixture.source_id, "move.md");
  const nlohmann::json moved = transfer(fixture, "move.md", "Nested", kMoveOptions, &error);
  ASSERT_EQ(error, VXCORE_OK);
  ASSERT_EQ(moved["status"], "moved");
  ASSERT_FALSE(path_exists(fixture.source_path + "/move.md"));
  const nlohmann::json move_after =
      node_config(fixture.context, fixture.destination_id, "Nested/move.md");
  ASSERT_EQ(move_after["createdUtc"], move_before["createdUtc"]);
  ASSERT_EQ(move_after["modifiedUtc"], move_before["modifiedUtc"]);
  ASSERT_NE(move_after["id"], move_before["id"]);

  vxcore_string_free(source_file_id);
  return 0;
}

int check_recursive_folder_fidelity_and_assets(bool legacy) {
  Fixture fixture("node_transfer_folder");
  ASSERT(fixture.valid());
  char *folder_id = nullptr;
  ASSERT_EQ(
      vxcore_folder_create_path(fixture.context, fixture.source_id, "Project/Sub", &folder_id),
      VXCORE_OK);
  vxcore_string_free(folder_id);
  char *file_id = nullptr;
  ASSERT_EQ(
      vxcore_file_create(fixture.context, fixture.source_id, "Project/Sub", "note.md", &file_id),
      VXCORE_OK);
  ASSERT_EQ(vxcore_tag_create_path(fixture.context, fixture.source_id, "work/project"), VXCORE_OK);
  ASSERT_EQ(vxcore_file_update_tags(fixture.context, fixture.source_id, "Project/Sub/note.md",
                                    "[\"project\"]"),
            VXCORE_OK);
  ASSERT_EQ(vxcore_node_update_metadata(fixture.context, fixture.source_id, "Project",
                                        "{\"color\":\"blue\",\"custom\":7}"),
            VXCORE_OK);
  const std::string note_path = fixture.source_path + "/Project/Sub/note.md";
  write_file(note_path, "![asset](vx_assets/" + std::string(file_id) +
                            "/image.png)\n[relative](../outside.txt)\n");
  write_file(fixture.source_path + "/Project/.hidden", "hidden\n");
  write_file(fixture.source_path + "/outside.txt", "must not travel\n");
  const std::string asset_root =
      fixture.source_path + "/Project/Sub/vx_assets/" + std::string(file_id);
  create_directory(asset_root);
  write_file(asset_root + "/image.png", "PNG");
  write_file(asset_root + "/comments.json", "{\"comments\":[1]}");
  const std::string attachment_source = fixture.source_path + "/spec.pdf";
  write_file(attachment_source, "PDF");
  char *buffer_id = nullptr;
  ASSERT_EQ(
      vxcore_buffer_open(fixture.context, fixture.source_id, "Project/Sub/note.md", &buffer_id),
      VXCORE_OK);
  char *attachment_name = nullptr;
  ASSERT_EQ(vxcore_buffer_insert_attachment(fixture.context, buffer_id, attachment_source.c_str(),
                                            &attachment_name),
            VXCORE_OK);
  ASSERT_EQ(std::string(attachment_name), "spec.pdf");
  vxcore_string_free(attachment_name);
  ASSERT_EQ(vxcore_buffer_close(fixture.context, buffer_id), VXCORE_OK);
  vxcore_string_free(buffer_id);
  buffer_id = nullptr;

  if (legacy) {
    ASSERT_EQ(vxcore_notebook_close(fixture.context, fixture.source_id), VXCORE_OK);
    vxcore_string_free(fixture.source_id);
    fixture.source_id = nullptr;
    const std::string config_path =
        fixture.source_path + "/vx_notebook/contents/Project/Sub/vx.json";
    nlohmann::json config = nlohmann::json::parse(read_file_bytes(config_path));
    config.at("files").at(0)["attachments"] =
        nlohmann::json::array({"Project/Sub/vx_assets/" + std::string(file_id) + "/spec.pdf"});
    write_file(config_path, config.dump());
    ASSERT_EQ(
        vxcore_notebook_open(fixture.context, fixture.source_path.c_str(), &fixture.source_id),
        VXCORE_OK);
    ASSERT_EQ(node_config(fixture.context, fixture.source_id, "Project/Sub/note.md")["attachments"],
              nlohmann::json::array({"spec.pdf"}));
  }

  VxCoreError error = VXCORE_ERR_UNKNOWN;
  const nlohmann::json result = transfer(fixture, "Project", ".", kCopyOptions, &error);
  ASSERT_EQ(error, VXCORE_OK);
  ASSERT_EQ(result["status"], "copied");
  ASSERT(path_exists(fixture.destination_path + "/Project/.hidden"));
  ASSERT_FALSE(path_exists(fixture.destination_path + "/outside.txt"));
  const nlohmann::json folder = node_config(fixture.context, fixture.destination_id, "Project");
  ASSERT_EQ(folder["metadata"]["color"], "blue");
  const nlohmann::json file =
      node_config(fixture.context, fixture.destination_id, "Project/Sub/note.md");
  ASSERT_NE(file["id"].get<std::string>(), std::string(file_id));
  ASSERT_EQ(file["tags"][0], "project");
  ASSERT_EQ(file["attachments"], nlohmann::json::array({"spec.pdf"}));
  const std::string destination_config_path =
      fixture.destination_path + "/vx_notebook/contents/Project/Sub/vx.json";
  ASSERT_EQ(nlohmann::json::parse(read_file_bytes(destination_config_path))
                .at("files")
                .at(0)
                .at("attachments"),
            nlohmann::json::array({"spec.pdf"}));
  const std::string destination_assets =
      fixture.destination_path + "/Project/Sub/vx_assets/" + file["id"].get<std::string>();
  ASSERT(path_exists(destination_assets + "/image.png"));
  ASSERT(path_exists(destination_assets + "/spec.pdf"));
  ASSERT(path_exists(destination_assets + "/comments.json"));
  std::ifstream note(utf8_to_fs_path(fixture.destination_path + "/Project/Sub/note.md"),
                     std::ios::binary);
  const std::string content((std::istreambuf_iterator<char>(note)),
                            std::istreambuf_iterator<char>());
  ASSERT(content.find("vx_assets/" + file["id"].get<std::string>() + "/image.png") !=
         std::string::npos);
  ASSERT(content.find("[relative](../outside.txt)") != std::string::npos);

  ASSERT_EQ(vxcore_buffer_open(fixture.context, fixture.destination_id, "Project/Sub/note.md",
                               &buffer_id),
            VXCORE_OK);
  char *renamed_name = nullptr;
  ASSERT_EQ(vxcore_buffer_rename_attachment(fixture.context, buffer_id, "spec.pdf",
                                            "renamed-spec.pdf", &renamed_name),
            VXCORE_OK);
  ASSERT_EQ(std::string(renamed_name), "renamed-spec.pdf");
  vxcore_string_free(renamed_name);
  ASSERT_FALSE(path_exists(destination_assets + "/spec.pdf"));
  ASSERT_EQ(read_file_bytes(destination_assets + "/renamed-spec.pdf"), "PDF");
  ASSERT_EQ(
      node_config(fixture.context, fixture.destination_id, "Project/Sub/note.md")["attachments"],
      nlohmann::json::array({"renamed-spec.pdf"}));
  ASSERT_EQ(nlohmann::json::parse(read_file_bytes(destination_config_path))
                .at("files")
                .at(0)
                .at("attachments"),
            nlohmann::json::array({"renamed-spec.pdf"}));
  ASSERT_EQ(vxcore_buffer_delete_attachment(fixture.context, buffer_id, "renamed-spec.pdf"),
            VXCORE_OK);
  ASSERT_FALSE(path_exists(destination_assets + "/renamed-spec.pdf"));
  char *attachments_json = nullptr;
  ASSERT_EQ(vxcore_buffer_list_attachments(fixture.context, buffer_id, &attachments_json),
            VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(attachments_json), nlohmann::json::array());
  vxcore_string_free(attachments_json);
  ASSERT_FALSE(nlohmann::json::parse(read_file_bytes(destination_config_path))
                   .at("files")
                   .at(0)
                   .contains("attachments"));
  ASSERT_EQ(vxcore_buffer_close(fixture.context, buffer_id), VXCORE_OK);
  vxcore_string_free(buffer_id);
  ASSERT_EQ(read_file_bytes(asset_root + "/spec.pdf"), "PDF");
  ASSERT_EQ(read_file_bytes(destination_assets + "/image.png"), "PNG");
  ASSERT_EQ(read_file_bytes(destination_assets + "/comments.json"), "{\"comments\":[1]}");
  ASSERT_EQ(read_file_bytes(fixture.destination_path + "/Project/Sub/note.md"), content);

  char *tags_json = nullptr;
  ASSERT_EQ(vxcore_tag_list(fixture.context, fixture.destination_id, &tags_json), VXCORE_OK);
  const nlohmann::json tags = nlohmann::json::parse(tags_json);
  vxcore_string_free(tags_json);
  ASSERT(std::any_of(tags.begin(), tags.end(), [](const nlohmann::json &tag) {
    return tag.value("name", "") == "project" && tag.value("parent", "") == "work";
  }));
  vxcore_string_free(file_id);
  return 0;
}

int test_recursive_folder_fidelity_and_assets() {
  const int result = check_recursive_folder_fidelity_and_assets(false);
  return result == 0 ? check_recursive_folder_fidelity_and_assets(true) : result;
}

int test_cancellation_and_rejections() {
  Fixture fixture("node_transfer_rejections");
  ASSERT(fixture.valid());
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(fixture.context, fixture.source_id, ".", "cancel.md", &file_id),
            VXCORE_OK);
  vxcore_string_free(file_id);
  write_file(fixture.source_path + "/cancel.md", std::string(128 * 1024, 'x'));

  VxCoreNodeTransferHandle handle = nullptr;
  ASSERT_EQ(vxcore_node_transfer_prepare(fixture.context, fixture.source_id, "cancel.md",
                                         fixture.destination_id, ".", kCopyOptions, cancel_progress,
                                         nullptr, &handle),
            VXCORE_ERR_CANCELLED);
  ASSERT_NULL(handle);
  ASSERT_FALSE(path_exists(fixture.destination_path + "/cancel.md"));

  ASSERT_EQ(
      vxcore_node_transfer_prepare(fixture.context, fixture.source_id, ".", fixture.destination_id,
                                   ".", kCopyOptions, nullptr, nullptr, &handle),
      VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_node_transfer_prepare(fixture.context, fixture.source_id, "../cancel.md",
                                         fixture.destination_id, ".", kCopyOptions, nullptr,
                                         nullptr, &handle),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_notebook_set_read_only(fixture.context, fixture.destination_id, true),
            VXCORE_OK);
  ASSERT_EQ(vxcore_node_transfer_prepare(fixture.context, fixture.source_id, "cancel.md",
                                         fixture.destination_id, ".", kCopyOptions, nullptr,
                                         nullptr, &handle),
            VXCORE_ERR_READ_ONLY);

  ASSERT_EQ(vxcore_notebook_set_read_only(fixture.context, fixture.destination_id, false),
            VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_set_read_only(fixture.context, fixture.source_id, true), VXCORE_OK);
  ASSERT_EQ(vxcore_node_transfer_prepare(fixture.context, fixture.source_id, "cancel.md",
                                         fixture.destination_id, ".", kMoveOptions, nullptr,
                                         nullptr, &handle),
            VXCORE_ERR_READ_ONLY);
  ASSERT_EQ(vxcore_node_transfer_prepare(fixture.context, fixture.source_id, "cancel.md",
                                         fixture.destination_id, ".", kCopyOptions, nullptr,
                                         nullptr, &handle),
            VXCORE_OK);
  vxcore_node_transfer_free(fixture.context, handle);
  ASSERT_FALSE(path_exists(fixture.destination_path + "/cancel.md"));
  ASSERT_EQ(vxcore_notebook_set_read_only(fixture.context, fixture.source_id, false), VXCORE_OK);
  const nlohmann::json before_invalid_update =
      node_config(fixture.context, fixture.source_id, "cancel.md");
  ASSERT_EQ(vxcore_file_update_attachments(fixture.context, fixture.source_id, "cancel.md",
                                           "[\"../escape.bin\"]"),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(node_config(fixture.context, fixture.source_id, "cancel.md"), before_invalid_update);

  // Persisted unsafe metadata must still reach and fail the transfer validation gate.
  ASSERT_EQ(vxcore_notebook_close(fixture.context, fixture.source_id), VXCORE_OK);
  vxcore_string_free(fixture.source_id);
  fixture.source_id = nullptr;
  const std::string config_path = fixture.source_path + "/vx_notebook/contents/vx.json";
  nlohmann::json config = nlohmann::json::parse(read_file_bytes(config_path));
  config.at("files").at(0)["attachments"] = nlohmann::json::array({"../escape.bin"});
  write_file(config_path, config.dump());
  ASSERT_EQ(vxcore_notebook_open(fixture.context, fixture.source_path.c_str(), &fixture.source_id),
            VXCORE_OK);
  ASSERT_EQ(node_config(fixture.context, fixture.source_id, "cancel.md")["attachments"],
            nlohmann::json::array({"../escape.bin"}));
  char *listed = nullptr;
  ASSERT_EQ(vxcore_node_list_attachments(fixture.context, fixture.source_id, "cancel.md", &listed),
            VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(listed), nlohmann::json::array({"escape.bin"}));
  vxcore_string_free(listed);
  handle = nullptr;
  ASSERT_EQ(vxcore_node_transfer_prepare(fixture.context, fixture.source_id, "cancel.md",
                                         fixture.destination_id, ".", kCopyOptions, nullptr,
                                         nullptr, &handle),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_NULL(handle);
  ASSERT_FALSE(path_exists(fixture.destination_path + "/cancel.md"));

  const std::string raw_path = get_test_path("node_transfer_raw");
  cleanup_test_dir(raw_path);
  char *raw_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(fixture.context, raw_path.c_str(), "{\"name\":\"Raw\"}",
                                   VXCORE_NOTEBOOK_RAW, &raw_id),
            VXCORE_OK);
  ASSERT_EQ(vxcore_node_transfer_prepare(fixture.context, fixture.source_id, "cancel.md", raw_id,
                                         ".", kCopyOptions, nullptr, nullptr, &handle),
            VXCORE_ERR_UNSUPPORTED);
  char *raw_file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(fixture.context, raw_id, ".", "raw.md", &raw_file_id), VXCORE_OK);
  vxcore_string_free(raw_file_id);
  ASSERT_EQ(vxcore_node_transfer_prepare(fixture.context, raw_id, "raw.md", fixture.destination_id,
                                         ".", kCopyOptions, nullptr, nullptr, &handle),
            VXCORE_ERR_UNSUPPORTED);
  char *finalize_result = nullptr;
  ASSERT_EQ(vxcore_node_finalize_transfer_move(fixture.context, "{}", &finalize_result),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_NULL(finalize_result);
  vxcore_string_free(raw_id);
  cleanup_test_dir(raw_path);
  return 0;
}

int test_prepare_fingerprint_free_and_events() {
  Fixture fixture("node_transfer_fingerprint");
  ASSERT(fixture.valid());
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(fixture.context, fixture.source_id, ".", "change.md", &file_id),
            VXCORE_OK);
  vxcore_string_free(file_id);
  write_file(fixture.source_path + "/change.md", "before\n");

  EventCapture capture;
  ASSERT_EQ(vxcore_on_event(fixture.context, "file.created", capture_event, &capture), VXCORE_OK);
  ASSERT_EQ(vxcore_on_event(fixture.context, "file.deleted", capture_event, &capture), VXCORE_OK);
  ASSERT_EQ(vxcore_on_event(fixture.context, "folder.config_changed", capture_event, &capture),
            VXCORE_OK);

  VxCoreNodeTransferHandle handle = nullptr;
  ASSERT_EQ(vxcore_node_transfer_prepare(fixture.context, fixture.source_id, "change.md",
                                         fixture.destination_id, ".", kCopyOptions, nullptr,
                                         nullptr, &handle),
            VXCORE_OK);
  write_file(fixture.source_path + "/change.md", "after\n");
  char *result_json = nullptr;
  ASSERT_EQ(vxcore_node_transfer_commit(fixture.context, handle, &result_json),
            VXCORE_ERR_INVALID_STATE);
  ASSERT_NULL(result_json);
  ASSERT_FALSE(path_exists(fixture.destination_path + "/change.md"));
  ASSERT_EQ(count_event(capture, "file.created"), 0);
  ASSERT_EQ(count_event(capture, "file.deleted"), 0);

  ASSERT_EQ(vxcore_node_transfer_prepare(fixture.context, fixture.source_id, "change.md",
                                         fixture.destination_id, ".", kCopyOptions, nullptr,
                                         nullptr, &handle),
            VXCORE_OK);
  vxcore_node_transfer_free(fixture.context, handle);
  ASSERT_FALSE(path_exists(fixture.destination_path + "/change.md"));

  const nlohmann::json moved = transfer(fixture, "change.md", ".", kMoveOptions);
  ASSERT_EQ(moved["status"], "moved");
  ASSERT_EQ(count_event(capture, "file.created"), 1);
  ASSERT_EQ(count_event(capture, "file.deleted"), 1);
  ASSERT_EQ(count_event(capture, "folder.config_changed"), 2);
  return 0;
}

int test_events_are_explicitly_deferred_and_one_shot() {
  Fixture fixture("node_transfer_deferred_events");
  ASSERT(fixture.valid());
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(fixture.context, fixture.source_id, ".", "deferred.md", &file_id),
            VXCORE_OK);
  vxcore_string_free(file_id);
  write_file(fixture.source_path + "/deferred.md", "deferred\n");
  EventCapture capture;
  ASSERT_EQ(vxcore_on_event(fixture.context, "folder.config_changed", capture_event, &capture),
            VXCORE_OK);
  ASSERT_EQ(vxcore_on_event(fixture.context, "file.created", capture_event, &capture), VXCORE_OK);

  VxCoreNodeTransferHandle handle = nullptr;
  ASSERT_EQ(vxcore_node_transfer_prepare(fixture.context, fixture.source_id, "deferred.md",
                                         fixture.destination_id, ".", kCopyOptions, nullptr,
                                         nullptr, &handle),
            VXCORE_OK);
  char *result_json = nullptr;
  ASSERT_EQ(vxcore_node_transfer_commit(fixture.context, handle, &result_json), VXCORE_OK);
  ASSERT_NOT_NULL(result_json);
  const nlohmann::json result = nlohmann::json::parse(result_json);
  vxcore_string_free(result_json);
  ASSERT(result.contains("eventBatchId"));
  ASSERT(capture.names.empty());
  const std::string batch_id = result["eventBatchId"].get<std::string>();
  ASSERT_EQ(vxcore_node_transfer_dispatch_events(fixture.context, batch_id.c_str()), VXCORE_OK);
  ASSERT_EQ(capture.names.size(), 2u);
  ASSERT_EQ(capture.names[0], "folder.config_changed");
  ASSERT_EQ(capture.names[1], "file.created");
  ASSERT_EQ(vxcore_node_transfer_dispatch_events(fixture.context, batch_id.c_str()),
            VXCORE_ERR_NOT_FOUND);
  return 0;
}

int test_move_finalize_retries_source_only() {
  Fixture fixture("node_transfer_finalize");
  ASSERT(fixture.valid());
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(fixture.context, fixture.source_id, ".", "finalize.md", &file_id),
            VXCORE_OK);
  vxcore_string_free(file_id);
  write_file(fixture.source_path + "/finalize.md", "finalize\n");

  VxCoreError transfer_error = VXCORE_ERR_UNKNOWN;
  const nlohmann::json retained =
      transfer(fixture, "finalize.md", ".", kMovePostCommitFaultOptions, &transfer_error);
  ASSERT_EQ(transfer_error, VXCORE_OK);
  ASSERT_EQ(retained["status"], "moveRecoveryRequired");
  ASSERT(retained.contains("resumeToken"));
  ASSERT(path_exists(fixture.source_path + "/finalize.md"));
  ASSERT(path_exists(fixture.destination_path + "/finalize.md"));

  char *result_json = nullptr;
  const std::string token = retained["resumeToken"].dump();
  ASSERT_EQ(vxcore_node_finalize_transfer_move(fixture.context, token.c_str(), &result_json),
            VXCORE_OK);
  ASSERT_NOT_NULL(result_json);
  const nlohmann::json finalized = nlohmann::json::parse(result_json);
  vxcore_string_free(result_json);
  ASSERT_EQ(finalized["status"], "moved");
  ASSERT(finalized.contains("eventBatchId"));
  const std::string event_batch_id = finalized["eventBatchId"].get<std::string>();
  ASSERT_EQ(vxcore_node_transfer_dispatch_events(fixture.context, event_batch_id.c_str()),
            VXCORE_OK);
  ASSERT_FALSE(path_exists(fixture.source_path + "/finalize.md"));
  ASSERT(path_exists(fixture.destination_path + "/finalize.md"));
  ASSERT_FALSE(path_exists(fixture.destination_path + "/finalize (2).md"));
  return 0;
}

int test_copy_post_commit_fault_returns_durable_fact() {
  Fixture fixture("node_transfer_copy_post_commit");
  ASSERT(fixture.valid());
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(fixture.context, fixture.source_id, ".", "durable.md", &file_id),
            VXCORE_OK);
  vxcore_string_free(file_id);
  write_file(fixture.source_path + "/durable.md", "durable\n");
  VxCoreError error = VXCORE_ERR_UNKNOWN;
  const nlohmann::json result =
      transfer(fixture, "durable.md", ".", kCopyPostCommitFaultOptions, &error);
  ASSERT_EQ(error, VXCORE_OK);
  ASSERT_EQ(result["status"], "copied");
  ASSERT_EQ(result.value("recoveryDeferred", false), true);
  ASSERT(path_exists(fixture.destination_path + "/durable.md"));
  ASSERT_FALSE(path_exists(fixture.destination_path + "/durable (2).md"));
  return 0;
}

int test_result_handoff_faults_publish_none_or_return_durable_fact() {
  Fixture fixture("node_transfer_result_handoff");
  ASSERT(fixture.valid());
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(fixture.context, fixture.source_id, ".", "before.md", &file_id),
            VXCORE_OK);
  vxcore_string_free(file_id);
  write_file(fixture.source_path + "/before.md", "before\n");

  VxCoreNodeTransferHandle handle = nullptr;
  ASSERT_EQ(vxcore_node_transfer_prepare(
                fixture.context, fixture.source_id, "before.md", fixture.destination_id, ".",
                kCopyResultBeforeCommitFaultOptions, nullptr, nullptr, &handle),
            VXCORE_OK);
  char *result_json = nullptr;
  ASSERT_EQ(vxcore_node_transfer_commit(fixture.context, handle, &result_json),
            VXCORE_ERR_OUT_OF_MEMORY);
  ASSERT_NULL(result_json);
  ASSERT_FALSE(path_exists(fixture.destination_path + "/before.md"));

  ASSERT_EQ(vxcore_file_create(fixture.context, fixture.source_id, ".", "after.md", &file_id),
            VXCORE_OK);
  vxcore_string_free(file_id);
  write_file(fixture.source_path + "/after.md", "after\n");
  ASSERT_EQ(vxcore_node_transfer_prepare(
                fixture.context, fixture.source_id, "after.md", fixture.destination_id, ".",
                kMoveResultAfterCommitFaultOptions, nullptr, nullptr, &handle),
            VXCORE_OK);
  ASSERT_EQ(vxcore_node_transfer_commit(fixture.context, handle, &result_json), VXCORE_OK);
  ASSERT_NOT_NULL(result_json);
  nlohmann::json retained = nlohmann::json::parse(result_json);
  vxcore_string_free(result_json);
  ASSERT(retained.contains("status"));
  ASSERT_EQ(retained["status"], "moveRecoveryRequired");
  ASSERT_EQ(retained.value("recoveryDeferred", false), true);
  ASSERT(retained.contains("resumeToken"));
  ASSERT(path_exists(fixture.source_path + "/after.md"));
  ASSERT(path_exists(fixture.destination_path + "/after.md"));

  const std::string batch_id = retained["eventBatchId"].get<std::string>();
  ASSERT_EQ(vxcore_node_transfer_dispatch_events(fixture.context, batch_id.c_str()), VXCORE_OK);
  const std::string token = retained["resumeToken"].dump();
  result_json = nullptr;
  ASSERT_EQ(vxcore_node_finalize_transfer_move(fixture.context, token.c_str(), &result_json),
            VXCORE_OK);
  ASSERT_NOT_NULL(result_json);
  const nlohmann::json finalized = nlohmann::json::parse(result_json);
  vxcore_string_free(result_json);
  ASSERT_EQ(finalized["status"], "moved");
  ASSERT_FALSE(path_exists(fixture.source_path + "/after.md"));
  ASSERT(path_exists(fixture.destination_path + "/after.md"));
  ASSERT_FALSE(path_exists(fixture.destination_path + "/after (2).md"));
  return 0;
}

int test_source_retained_requires_verified_rollback() {
  Fixture fixture("node_transfer_verified_source_rollback");
  ASSERT(fixture.valid());
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(fixture.context, fixture.source_id, ".", "retained.md", &file_id),
            VXCORE_OK);
  vxcore_string_free(file_id);
  write_file(fixture.source_path + "/retained.md", "retained bytes\n");
  const nlohmann::json before = node_config(fixture.context, fixture.source_id, "retained.md");

  VxCoreError error = VXCORE_ERR_UNKNOWN;
  const nlohmann::json result =
      transfer(fixture, "retained.md", ".", kMoveSourceRemovalFaultOptions, &error);
  ASSERT_EQ(error, VXCORE_OK);
  ASSERT_EQ(result["status"], "copiedSourceRetained");
  ASSERT(result.contains("resumeToken"));
  ASSERT(path_exists(fixture.source_path + "/retained.md"));
  ASSERT_EQ(node_config(fixture.context, fixture.source_id, "retained.md"), before);
  ASSERT(path_exists(fixture.destination_path + "/retained.md"));
  char *result_json = nullptr;
  const std::string token = result["resumeToken"].dump();
  ASSERT_EQ(vxcore_node_finalize_transfer_move(fixture.context, token.c_str(), &result_json),
            VXCORE_OK);
  vxcore_string_free(result_json);
  ASSERT_FALSE(path_exists(fixture.source_path + "/retained.md"));
  ASSERT_FALSE(path_exists(fixture.destination_path + "/retained (2).md"));
  return 0;
}

int test_uncertain_source_rollback_never_reports_retained() {
  const std::vector<std::pair<std::string, const char *>> faults = {
      {"content", kMoveSourceRollbackContentFaultOptions},
      {"parent", kMoveSourceRollbackParentFaultOptions}};
  for (const auto &fault : faults) {
    Fixture fixture("node_transfer_uncertain_source_" + fault.first);
    ASSERT(fixture.valid());
    const std::string name = fault.first + ".md";
    char *file_id = nullptr;
    ASSERT_EQ(vxcore_file_create(fixture.context, fixture.source_id, ".", name.c_str(), &file_id),
              VXCORE_OK);
    vxcore_string_free(file_id);
    write_file(fixture.source_path + "/" + name, "uncertain bytes\n");

    VxCoreError error = VXCORE_ERR_UNKNOWN;
    const nlohmann::json result = transfer(fixture, name, ".", fault.second, &error);
    ASSERT_EQ(error, VXCORE_OK);
    ASSERT_NE(result["status"], "copiedSourceRetained");
    ASSERT_EQ(result["status"], "moveRecoveryRequired");
    ASSERT_EQ(result.value("recoveryRequired", false), true);
    ASSERT(result.contains("resumeToken"));
    ASSERT(result["resumeToken"].contains("sourceRecoveryId"));
    ASSERT(path_exists(fixture.destination_path + "/" + name));

    const std::string transfer_root = fixture.source_path + "/vx_notebook/vx_transfer";
    bool journal_found = false;
    for (const auto &entry : std::filesystem::directory_iterator(utf8_to_fs_path(transfer_root))) {
      if (std::filesystem::exists(entry.path() / "journal.json")) {
        journal_found = true;
        break;
      }
    }
    ASSERT(journal_found);
  }
  return 0;
}

int test_exception_after_source_quarantine_requires_recovery() {
  Fixture fixture("node_transfer_quarantine_exception");
  ASSERT(fixture.valid());
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(fixture.context, fixture.source_id, ".", "quarantine.md", &file_id),
            VXCORE_OK);
  write_file(fixture.source_path + "/quarantine.md", "source bytes\n");
  const std::string attachment_source = fixture.source_path + "/asset.bin";
  write_file(attachment_source, "asset bytes\n");
  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(fixture.context, fixture.source_id, "quarantine.md", &buffer_id),
            VXCORE_OK);
  char *attachment_name = nullptr;
  ASSERT_EQ(vxcore_buffer_insert_attachment(fixture.context, buffer_id, attachment_source.c_str(),
                                            &attachment_name),
            VXCORE_OK);
  ASSERT_EQ(std::string(attachment_name), "asset.bin");
  vxcore_string_free(attachment_name);
  ASSERT_EQ(vxcore_buffer_close(fixture.context, buffer_id), VXCORE_OK);
  vxcore_string_free(buffer_id);
  const std::string asset_root = fixture.source_path + "/vx_assets/" + std::string(file_id);
  vxcore_string_free(file_id);

  VxCoreError error = VXCORE_ERR_UNKNOWN;
  const nlohmann::json result =
      transfer(fixture, "quarantine.md", ".", kMoveSourceQuarantineExceptionOptions, &error);
  ASSERT_EQ(error, VXCORE_OK);
  ASSERT_EQ(result["status"], "moveRecoveryRequired");
  ASSERT_NE(result["status"], "copiedSourceRetained");
  ASSERT(result.contains("resumeToken"));
  ASSERT(path_exists(fixture.destination_path + "/quarantine.md"));
  ASSERT_FALSE(path_exists(fixture.destination_path + "/quarantine (2).md"));
  const std::string transfer_root = fixture.source_path + "/vx_notebook/vx_transfer";
  bool journal_found = false;
  for (const auto &entry : std::filesystem::directory_iterator(utf8_to_fs_path(transfer_root))) {
    if (std::filesystem::exists(entry.path() / "journal.json")) {
      journal_found = true;
      break;
    }
  }
  ASSERT(journal_found);

  ASSERT_EQ(vxcore_notebook_close(fixture.context, fixture.source_id), VXCORE_OK);
  char *reopened_id = nullptr;
  ASSERT_EQ(vxcore_notebook_open(fixture.context, fixture.source_path.c_str(), &reopened_id),
            VXCORE_OK);
  vxcore_string_free(reopened_id);
  ASSERT(path_exists(fixture.source_path + "/quarantine.md"));
  ASSERT(path_exists(asset_root + "/asset.bin"));
  ASSERT(path_exists(fixture.destination_path + "/quarantine.md"));
  ASSERT_FALSE(path_exists(fixture.destination_path + "/quarantine (2).md"));
  return 0;
}

int test_destination_case_comparer_and_no_replace_race() {
  Fixture fixture("node_transfer_destination_conflicts");
  ASSERT(fixture.valid());
  const bool case_insensitive = filesystem_is_case_insensitive(fixture.destination_path);

  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(fixture.context, fixture.destination_id, ".", "Case.md", &file_id),
            VXCORE_OK);
  vxcore_string_free(file_id);
  write_file(fixture.destination_path + "/Case.md", "existing case bytes\n");
  ASSERT_EQ(vxcore_file_create(fixture.context, fixture.source_id, ".", "case.md", &file_id),
            VXCORE_OK);
  vxcore_string_free(file_id);
  write_file(fixture.source_path + "/case.md", "copied case bytes\n");

  VxCoreError error = VXCORE_ERR_UNKNOWN;
  const nlohmann::json case_result = transfer(fixture, "case.md", ".", kCopyOptions, &error);
  ASSERT_EQ(error, VXCORE_OK);
  ASSERT_EQ(case_result["destinationRelativePath"], case_insensitive ? "case (2).md" : "case.md");
  ASSERT_EQ(read_file_bytes(fixture.destination_path + "/Case.md"), "existing case bytes\n");

  ASSERT_EQ(vxcore_file_create(fixture.context, fixture.source_id, ".", "race.md", &file_id),
            VXCORE_OK);
  vxcore_string_free(file_id);
  write_file(fixture.source_path + "/race.md", "transfer bytes\n");
  const nlohmann::json race_result =
      transfer(fixture, "race.md", ".", kCopyDestinationRaceOptions, &error);
  ASSERT(race_result.is_null());
  ASSERT_EQ(error, VXCORE_ERR_ALREADY_EXISTS);
  ASSERT_EQ(read_file_bytes(fixture.destination_path + "/race.md"), "external contender\n");
  ASSERT_FALSE(path_exists(fixture.destination_path + "/race (2).md"));
  ASSERT(node_config(fixture.context, fixture.destination_id, "race.md").is_null());
  return 0;
}

int test_recovery_fail_closed_and_committed_cleanup() {
  const std::string path = get_test_path("node_transfer_recovery");
  cleanup_test_dir(path);
  VxCoreContextHandle context = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &context), VXCORE_OK);
  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(context, path.c_str(), "{\"name\":\"Recovery\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(context, notebook_id, ".", "source.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);
  write_file(path + "/source.md", "source\n");
  ASSERT_EQ(vxcore_notebook_close(context, notebook_id), VXCORE_OK);
  vxcore_string_free(notebook_id);

  const std::string bad_dir = path + "/vx_notebook/vx_transfer/bad";
  create_directory(bad_dir);
  write_file(bad_dir + "/journal.json", "not-json");
  ASSERT_EQ(vxcore_notebook_open(context, path.c_str(), &notebook_id), VXCORE_ERR_INVALID_STATE);
  ASSERT_NULL(notebook_id);
  ASSERT(path_exists(bad_dir + "/journal.json"));

  cleanup_test_dir(bad_dir);
  std::ifstream parent_file(utf8_to_fs_path(path + "/vx_notebook/contents/vx.json"),
                            std::ios::binary);
  const std::string parent_bytes((std::istreambuf_iterator<char>(parent_file)),
                                 std::istreambuf_iterator<char>());
  parent_file.close();
  std::ifstream config_file(utf8_to_fs_path(path + "/vx_notebook/config.json"), std::ios::binary);
  const std::string config_bytes((std::istreambuf_iterator<char>(config_file)),
                                 std::istreambuf_iterator<char>());
  config_file.close();

  const std::string source_recovery_dir = path + "/vx_notebook/vx_transfer/source-recovery";
  const std::string content_quarantine = source_recovery_dir + "/quarantine/content";
  create_directory(source_recovery_dir + "/quarantine");
  std::error_code rename_error;
  std::filesystem::rename(utf8_to_fs_path(path + "/source.md"), utf8_to_fs_path(content_quarantine),
                          rename_error);
  ASSERT_FALSE(rename_error);
  nlohmann::json source_recovery = {{"kind", "sourceRemoval"},
                                    {"phase", "quarantined"},
                                    {"sourceRelativePath", "source.md"},
                                    {"sourceParentPath", "."},
                                    {"sourceParentConfigBytes", parent_bytes},
                                    {"isFolder", false},
                                    {"contentSource", path + "/source.md"},
                                    {"contentQuarantine", content_quarantine},
                                    {"metadataSource", ""},
                                    {"metadataQuarantine", ""},
                                    {"assets", nlohmann::json::array()},
                                    {"ids", nlohmann::json::array()}};
  write_file(source_recovery_dir + "/journal.json", source_recovery.dump());
  ASSERT_EQ(vxcore_notebook_open(context, path.c_str(), &notebook_id), VXCORE_OK);
  ASSERT(path_exists(path + "/source.md"));
  ASSERT_FALSE(path_exists(source_recovery_dir));
  ASSERT_EQ(vxcore_notebook_close(context, notebook_id), VXCORE_OK);
  vxcore_string_free(notebook_id);
  notebook_id = nullptr;

  const std::string rollback_dir = path + "/vx_notebook/vx_transfer/rollback";
  create_directory(rollback_dir);
  write_file(path + "/partial.md", "partial\n");
  nlohmann::json rollback = {{"kind", "destination"},
                             {"phase", "published"},
                             {"destinationPath", "partial.md"},
                             {"contentTarget", path + "/partial.md"},
                             {"metadataTarget", ""},
                             {"destinationParentPath", "."},
                             {"isFolder", false},
                             {"parentConfigBytes", parent_bytes},
                             {"committedParentConfigBytes", "not-current"},
                             {"notebookConfigBytes", config_bytes},
                             {"assets", nlohmann::json::array()},
                             {"ids", nlohmann::json::array()},
                             {"createdTags", nlohmann::json::array()}};
  write_file(rollback_dir + "/journal.json", rollback.dump());
  ASSERT_EQ(vxcore_notebook_open(context, path.c_str(), &notebook_id), VXCORE_OK);
  ASSERT_FALSE(path_exists(path + "/partial.md"));
  ASSERT_FALSE(path_exists(rollback_dir));
  ASSERT_EQ(vxcore_notebook_close(context, notebook_id), VXCORE_OK);
  vxcore_string_free(notebook_id);
  notebook_id = nullptr;

  const std::string committed_dir = path + "/vx_notebook/vx_transfer/committed";
  create_directory(committed_dir);
  write_file(committed_dir + "/journal.json",
             nlohmann::json({{"kind", "destination"},
                             {"phase", "committed"},
                             {"destinationPath", "committed.md"},
                             {"destinationParentPath", "."},
                             {"contentTarget", path + "/committed.md"},
                             {"metadataTarget", ""},
                             {"isFolder", false},
                             {"parentConfigBytes", parent_bytes},
                             {"committedParentConfigBytes", parent_bytes},
                             {"assets", nlohmann::json::array()}})
                 .dump());
  ASSERT_EQ(vxcore_notebook_open(context, path.c_str(), &notebook_id), VXCORE_OK);
  ASSERT_FALSE(path_exists(committed_dir));

  vxcore_string_free(notebook_id);
  vxcore_context_destroy(context);
  cleanup_test_dir(path);
  return 0;
}

int test_recovery_rejects_malicious_paths_without_mutation() {
  const std::string path = get_test_path("node_transfer_malicious_recovery");
  const std::string outside = get_test_path("node_transfer_malicious_victim");
  cleanup_test_dir(path);
  cleanup_test_dir(outside);
  create_directory(outside);
  write_file(outside + "/victim.md", "untouched\n");
  VxCoreContextHandle context = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &context), VXCORE_OK);
  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(context, path.c_str(), "{\"name\":\"Recovery\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_close(context, notebook_id), VXCORE_OK);
  vxcore_string_free(notebook_id);
  notebook_id = nullptr;

  const std::string journal_dir = path + "/vx_notebook/vx_transfer/crafted";
  create_directory(journal_dir);
  nlohmann::json journal = {{"kind", "destination"},
                            {"phase", "published"},
                            {"destinationPath", "victim.md"},
                            {"destinationParentPath", "."},
                            {"contentTarget", outside + "/victim.md"},
                            {"metadataTarget", ""},
                            {"isFolder", false},
                            {"parentConfigBytes", "{}"},
                            {"committedParentConfigBytes", "{}"},
                            {"notebookConfigBytes", "{}"},
                            {"assets", nlohmann::json::array()},
                            {"ids", nlohmann::json::array()},
                            {"createdTags", nlohmann::json::array()}};
  write_file(journal_dir + "/journal.json", journal.dump());
  ASSERT_EQ(vxcore_notebook_open(context, path.c_str(), &notebook_id), VXCORE_ERR_INVALID_STATE);
  ASSERT_NULL(notebook_id);
  ASSERT(path_exists(outside + "/victim.md"));
  ASSERT(path_exists(journal_dir + "/journal.json"));

  vxcore_context_destroy(context);
  cleanup_test_dir(path);
  cleanup_test_dir(outside);
  return 0;
}

int test_context_destruction_discards_prepared_handles() {
  const std::string source_path = get_test_path("node_transfer_context_source");
  const std::string destination_path = get_test_path("node_transfer_context_destination");
  cleanup_test_dir(source_path);
  cleanup_test_dir(destination_path);
  VxCoreContextHandle context = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &context), VXCORE_OK);
  char *source_id = nullptr;
  char *destination_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(context, source_path.c_str(), "{\"name\":\"Source\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &source_id),
            VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_create(context, destination_path.c_str(), "{\"name\":\"Destination\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &destination_id),
            VXCORE_OK);
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(context, source_id, ".", "prepared.md", &file_id), VXCORE_OK);
  vxcore_string_free(file_id);
  write_file(source_path + "/prepared.md", "prepared\n");
  VxCoreNodeTransferHandle handle = nullptr;
  ASSERT_EQ(vxcore_node_transfer_prepare(context, source_id, "prepared.md", destination_id, ".",
                                         kCopyOptions, nullptr, nullptr, &handle),
            VXCORE_OK);
  ASSERT_NOT_NULL(handle);
  const std::string transfer_root = destination_path + "/vx_notebook/vx_transfer";
  ASSERT(path_exists(transfer_root));
  ASSERT_FALSE(std::filesystem::is_empty(utf8_to_fs_path(transfer_root)));

  vxcore_string_free(source_id);
  vxcore_string_free(destination_id);
  vxcore_context_destroy(context);
  ASSERT(std::filesystem::is_empty(utf8_to_fs_path(transfer_root)));
  cleanup_test_dir(source_path);
  cleanup_test_dir(destination_path);
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  std::cout << "Running node transfer tests..." << std::endl;
  RUN_TEST(test_file_copy_move_unicode_and_conflicts);
  RUN_TEST(test_recursive_folder_fidelity_and_assets);
  RUN_TEST(test_cancellation_and_rejections);
  RUN_TEST(test_prepare_fingerprint_free_and_events);
  RUN_TEST(test_events_are_explicitly_deferred_and_one_shot);
  RUN_TEST(test_move_finalize_retries_source_only);
  RUN_TEST(test_copy_post_commit_fault_returns_durable_fact);
  RUN_TEST(test_result_handoff_faults_publish_none_or_return_durable_fact);
  RUN_TEST(test_source_retained_requires_verified_rollback);
  RUN_TEST(test_uncertain_source_rollback_never_reports_retained);
  RUN_TEST(test_exception_after_source_quarantine_requires_recovery);
  RUN_TEST(test_destination_case_comparer_and_no_replace_race);
  RUN_TEST(test_recovery_fail_closed_and_committed_cleanup);
  RUN_TEST(test_recovery_rejects_malicious_paths_without_mutation);
  RUN_TEST(test_context_destruction_discards_prepared_handles);
  std::cout << "All node transfer tests passed!" << std::endl;
  return 0;
}
