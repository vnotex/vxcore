#include <cerrno>
#include <chrono>
#include <iterator>
#include <nlohmann/json.hpp>
#include <vector>

#include "test_utils.h"
#include "mobile/file_writer_mobile.h"
#include "vxcore/vxcore.h"
#include "vxcore/mobile/vxcore_mobile.h"

#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;

struct TempLeaf {
  fs::path path;
  explicit TempLeaf(const char *name) {
    static unsigned int sequence = 0;
    const auto parent = utf8_to_fs_path(get_test_path("atomic_buffer_save"));
    fs::create_directories(parent);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    do {
      path = parent / (std::string(name) + "-" + std::to_string(stamp) + "-" +
                       std::to_string(++sequence));
    } while (!fs::create_directory(path));
  }
  ~TempLeaf() { std::error_code ec; fs::remove_all(path, ec); }
};

struct CoreString {
  char *value = nullptr;
  ~CoreString() { vxcore_string_free(value); }
};

struct Fixture {
  TempLeaf leaf;
  VxCoreContextHandle context = nullptr;
  CoreString notebook, file, buffer;
  const std::string note_name = "笔记-é.md";
  explicit Fixture(const char *name) : leaf(name) {}
  ~Fixture() { vxcore_context_destroy(context); }
  fs::path NotePath() const { return leaf.path / "notebook" / utf8_to_fs_path(note_name); }
  int Initialize() {
    ASSERT_EQ(vxcore_context_create_with_paths(nullptr, fs_path_to_utf8(leaf.path / "app").c_str(),
        fs_path_to_utf8(leaf.path / "local").c_str(), &context), VXCORE_OK);
    ASSERT_EQ(vxcore_notebook_create(context, fs_path_to_utf8(leaf.path / "notebook").c_str(),
        "{\"name\":\"Atomic persistence\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook.value), VXCORE_OK);
    ASSERT_EQ(vxcore_file_create(context, notebook.value, ".", note_name.c_str(), &file.value), VXCORE_OK);
    ASSERT_EQ(vxcore_buffer_open(context, notebook.value, note_name.c_str(), &buffer.value), VXCORE_OK);
    return 0;
  }
};

std::vector<uint8_t> ReadBytes(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Unable to read fixture");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool HasTemporarySibling(const fs::path &path) {
  const auto prefix = path.filename().native() + fs::path(".tmp-").native();
  for (const auto &entry : fs::directory_iterator(path.parent_path())) {
    if (entry.path().filename().native().compare(0, prefix.size(), prefix) == 0) return true;
  }
  return false;
}

int CheckDirtyFailure(Fixture &fixture, const std::vector<uint8_t> &old_bytes) {
  const auto err = vxcore_buffer_save(fixture.context, fixture.buffer.value);
  ASSERT_EQ(err, VXCORE_ERR_IO);
  ASSERT_TRUE(ReadBytes(fixture.NotePath()) == old_bytes);
  int modified = 0;
  ASSERT_EQ(vxcore_buffer_is_modified(fixture.context, fixture.buffer.value, &modified), VXCORE_OK);
  ASSERT_EQ(modified, 1);
  VxCoreBufferState state;
  ASSERT_EQ(vxcore_buffer_get_state(fixture.context, fixture.buffer.value, &state), VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_SAVE_FAILED);
  ASSERT_FALSE(HasTemporarySibling(fixture.NotePath()));
  return 0;
}

int test_source_bytes_and_ids_survive_reopen() {
  Fixture fixture("roundtrip");
  ASSERT_EQ(fixture.Initialize(), 0);
  const std::string source = "# Unicode 笔记\r\nCafé and emoji \xf0\x9f\x8c\x8d\nlast line";
  ASSERT_EQ(vxcore_buffer_set_content_raw(fixture.context, fixture.buffer.value,
                                          source.data(), source.size()), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(fixture.context, fixture.buffer.value), VXCORE_OK);
  ASSERT_TRUE(ReadBytes(fixture.NotePath()) == std::vector<uint8_t>(source.begin(), source.end()));
  int modified = 1;
  ASSERT_EQ(vxcore_buffer_is_modified(fixture.context, fixture.buffer.value, &modified), VXCORE_OK);
  ASSERT_EQ(modified, 0);
  ASSERT_FALSE(HasTemporarySibling(fixture.NotePath()));
  ASSERT_EQ(vxcore_buffer_close(fixture.context, fixture.buffer.value), VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_close(fixture.context, fixture.notebook.value), VXCORE_OK);
  vxcore_context_destroy(fixture.context);
  fixture.context = nullptr;
  ASSERT_EQ(vxcore_context_create_with_paths(nullptr,
      fs_path_to_utf8(fixture.leaf.path / "app").c_str(),
      fs_path_to_utf8(fixture.leaf.path / "local").c_str(), &fixture.context), VXCORE_OK);
  CoreString notebook, node, buffer;
  ASSERT_EQ(vxcore_notebook_open(fixture.context,
      fs_path_to_utf8(fixture.leaf.path / "notebook").c_str(), &notebook.value), VXCORE_OK);
  ASSERT_EQ(std::string(notebook.value), std::string(fixture.notebook.value));
  ASSERT_EQ(vxcore_node_get_config(fixture.context, notebook.value, fixture.note_name.c_str(),
                                   &node.value), VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(node.value)["id"], fixture.file.value);
  ASSERT_EQ(vxcore_buffer_open(fixture.context, notebook.value, fixture.note_name.c_str(),
                               &buffer.value), VXCORE_OK);
  const void *body = nullptr;
  size_t size = 0;
  ASSERT_EQ(vxcore_buffer_get_content_raw(fixture.context, buffer.value, &body, &size), VXCORE_OK);
  ASSERT_EQ(size, source.size());
  ASSERT_TRUE(std::memcmp(body, source.data(), size) == 0);
  return 0;
}

int test_binary_vector_and_empty_publication() {
  Fixture fixture("binary");
  ASSERT_EQ(fixture.Initialize(), 0);
  const std::vector<uint8_t> binary{0, 1, 0xff, 0x80, '\r', '\n', 0, 'x'};
  ASSERT_EQ(vxcore_buffer_set_content_raw(fixture.context, fixture.buffer.value,
                                          binary.data(), binary.size()), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(fixture.context, fixture.buffer.value), VXCORE_OK);
  ASSERT_TRUE(ReadBytes(fixture.NotePath()) == binary);
  ASSERT_EQ(vxcore_buffer_reload(fixture.context, fixture.buffer.value), VXCORE_OK);
  const void *body = nullptr;
  size_t size = 0;
  ASSERT_EQ(vxcore_buffer_get_content_raw(fixture.context, fixture.buffer.value, &body, &size), VXCORE_OK);
  ASSERT_EQ(size, binary.size());
  ASSERT_TRUE(std::memcmp(body, binary.data(), size) == 0);
  ASSERT_EQ(vxcore_buffer_set_content_raw(fixture.context, fixture.buffer.value, nullptr, 0), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(fixture.context, fixture.buffer.value), VXCORE_OK);
  ASSERT_TRUE(ReadBytes(fixture.NotePath()).empty());
  // Recreating a missing ordinary backing file uses an exclusive sibling too.
  ASSERT_TRUE(fs::remove(fixture.NotePath()));
  ASSERT_EQ(vxcore_buffer_set_content_raw(fixture.context, fixture.buffer.value,
                                          binary.data(), binary.size()), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(fixture.context, fixture.buffer.value), VXCORE_OK);
  ASSERT_TRUE(ReadBytes(fixture.NotePath()) == binary);
  ASSERT_FALSE(HasTemporarySibling(fixture.NotePath()));
  return 0;
}

int test_prepublication_error_keeps_old_bytes() {
  TempLeaf leaf("sticky-error");
  const auto file = leaf.path / utf8_to_fs_path("原始.bin");
  write_file(fs_path_to_utf8(file), "original");
  const auto before = ReadBytes(file);
  {
    vxcore::MobileFileWriter writer(file);
    ASSERT_EQ(writer.Open(), VXCORE_OK);
    ASSERT_EQ(writer.Write("replacement", 11), VXCORE_OK);
    ASSERT_EQ(writer.Write(nullptr, 1), VXCORE_ERR_NULL_POINTER);
    ASSERT_EQ(writer.Commit(), VXCORE_ERR_NULL_POINTER);
    ASSERT_TRUE(ReadBytes(file) == before);
  }
  ASSERT_TRUE(ReadBytes(file) == before);
  ASSERT_FALSE(HasTemporarySibling(file));
  // A changed destination between admission and commit is not overwritten.
  {
    vxcore::MobileFileWriter writer(file);
    ASSERT_EQ(writer.Open(), VXCORE_OK);
    ASSERT_EQ(writer.Write("replacement", 11), VXCORE_OK);
    fs::rename(file, leaf.path / "original");
    write_file(fs_path_to_utf8(file), "external");
    ASSERT_EQ(writer.Commit(), VXCORE_ERR_IO);
  }
  ASSERT_TRUE(ReadBytes(leaf.path / "original") == before);
  ASSERT_TRUE(ReadBytes(file) == std::vector<uint8_t>({'e','x','t','e','r','n','a','l'}));
  ASSERT_FALSE(HasTemporarySibling(file));
  return 0;
}

int test_readonly_failure_retains_dirty_buffer() {
  Fixture fixture("readonly");
  ASSERT_EQ(fixture.Initialize(), 0);
  const auto old_bytes = ReadBytes(fixture.NotePath());
  ASSERT_EQ(vxcore_buffer_set_content_raw(fixture.context, fixture.buffer.value, "edited", 6), VXCORE_OK);
  ASSERT_EQ(::chmod(fixture.NotePath().c_str(), 0444), 0);
  const int result = CheckDirtyFailure(fixture, old_bytes);
  ASSERT_EQ(::chmod(fixture.NotePath().c_str(), 0644), 0);
  ASSERT_EQ(result, 0);
  ASSERT_EQ(vxcore_buffer_save(fixture.context, fixture.buffer.value), VXCORE_OK);
  ASSERT_TRUE(ReadBytes(fixture.NotePath()) == std::vector<uint8_t>({'e','d','i','t','e','d'}));
  return 0;
}

int test_links_and_nonregular_targets_are_rejected() {
  Fixture fixture("links");
  ASSERT_EQ(fixture.Initialize(), 0);
  const auto old_bytes = ReadBytes(fixture.NotePath());
  ASSERT_EQ(vxcore_buffer_set_content_raw(fixture.context, fixture.buffer.value, "edited", 6), VXCORE_OK);
  const auto hard_link = fixture.leaf.path / "hard-link";
  std::error_code hard_link_error;
  fs::create_hard_link(fixture.NotePath(), hard_link, hard_link_error);
  if (hard_link_error) {
#ifdef __ANDROID__
    // Android SELinux may forbid creating this fixture even in app-private storage.
    ASSERT_TRUE(hard_link_error == std::errc::permission_denied ||
                hard_link_error == std::errc::operation_not_permitted);
    std::cout << "Hardlink fixture unavailable under Android policy: "
              << hard_link_error.message() << '\n';
#else
    ASSERT_FALSE(hard_link_error);
#endif
  } else {
    ASSERT_EQ(CheckDirtyFailure(fixture, old_bytes), 0);
    ASSERT_TRUE(ReadBytes(hard_link) == old_bytes);
    ASSERT_TRUE(fs::equivalent(hard_link, fixture.NotePath()));
    fs::remove(hard_link);
  }
  const auto target = fixture.leaf.path / "symlink-target";
  fs::rename(fixture.NotePath(), target);
  std::error_code ec;
  fs::create_symlink(target, fixture.NotePath(), ec);
  if (!ec) {
    ASSERT_EQ(CheckDirtyFailure(fixture, old_bytes), 0);
    ASSERT_TRUE(fs::is_symlink(fs::symlink_status(fixture.NotePath())));
    ASSERT_TRUE(ReadBytes(target) == old_bytes);
    fs::remove(fixture.NotePath());
  } else {
    ASSERT_FALSE(ec);
  }
  fs::create_directory(fixture.NotePath());
  ASSERT_EQ(vxcore_buffer_save(fixture.context, fixture.buffer.value), VXCORE_ERR_IO);
  ASSERT_TRUE(fs::is_directory(fixture.NotePath()));
  ASSERT_TRUE(ReadBytes(target) == old_bytes);
  ASSERT_FALSE(HasTemporarySibling(fixture.NotePath()));
  fs::remove(fixture.NotePath());
  const int fifo_result = ::mkfifo(fixture.NotePath().c_str(), 0600);
#ifdef __ANDROID__
  if (fifo_result != 0 && (errno == EACCES || errno == EPERM)) {
    std::cout << "FIFO fixture unavailable under Android policy" << '\n';
  } else
#endif
  {
    ASSERT_EQ(fifo_result, 0);
    ASSERT_EQ(vxcore_buffer_save(fixture.context, fixture.buffer.value), VXCORE_ERR_IO);
    ASSERT_TRUE(fs::is_fifo(fixture.NotePath()));
    fs::remove(fixture.NotePath());
  }
  fs::rename(target, fixture.NotePath());
  ASSERT_EQ(vxcore_buffer_save(fixture.context, fixture.buffer.value), VXCORE_OK);
  return 0;
}

int test_asset_publication_preserves_bytes_and_names() {
  Fixture fixture("asset-publication");
  ASSERT_EQ(fixture.Initialize(), 0);
  const auto notebook_root = fixture.NotePath().parent_path();
  const auto note_before = ReadBytes(fixture.NotePath());
  const std::vector<uint8_t> first{0, 0xff, '\r', '\n', 0x80, 'a'};
  const std::vector<uint8_t> second{0x80, 0, 'b'};
  CoreString raw_first, raw_second, empty;
  ASSERT_EQ(vxcore_buffer_insert_asset_raw(fixture.context, fixture.buffer.value,
      "raw.bin", first.data(), first.size(), &raw_first.value), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_insert_asset_raw(fixture.context, fixture.buffer.value,
      "raw.bin", second.data(), second.size(), &raw_second.value), VXCORE_OK);
  ASSERT_EQ(utf8_to_fs_path(raw_first.value).filename(), fs::path("raw.bin"));
  ASSERT_EQ(utf8_to_fs_path(raw_second.value).filename(), fs::path("raw_1.bin"));
  ASSERT_TRUE(ReadBytes(notebook_root / utf8_to_fs_path(raw_first.value)) == first);
  ASSERT_TRUE(ReadBytes(notebook_root / utf8_to_fs_path(raw_second.value)) == second);
  ASSERT_EQ(vxcore_buffer_insert_asset_raw(fixture.context, fixture.buffer.value,
      "empty.bin", first.data(), 0, &empty.value), VXCORE_OK);
  ASSERT_TRUE(ReadBytes(notebook_root / utf8_to_fs_path(empty.value)).empty());

  // File input crosses multiple bounded chunks without text decoding/newline conversion.
  std::vector<uint8_t> source_bytes(2 * 64 * 1024 + 31);
  for (size_t i = 0; i < source_bytes.size(); ++i) source_bytes[i] = static_cast<uint8_t>(i);
  const auto source = fixture.leaf.path / "file.bin";
  {
    std::ofstream output(source, std::ios::binary);
    output.write(reinterpret_cast<const char *>(source_bytes.data()), source_bytes.size());
    output.close();
    ASSERT_TRUE(output.good());
  }
  CoreString file_first, file_second;
  ASSERT_EQ(vxcore_buffer_insert_asset(fixture.context, fixture.buffer.value,
      fs_path_to_utf8(source).c_str(), &file_first.value), VXCORE_OK);
  ASSERT_TRUE(ReadBytes(source) == source_bytes);
  {
    std::ofstream output(source, std::ios::binary);
    output.write(reinterpret_cast<const char *>(second.data()), second.size());
    output.close();
    ASSERT_TRUE(output.good());
  }
  ASSERT_EQ(vxcore_buffer_insert_asset(fixture.context, fixture.buffer.value,
      fs_path_to_utf8(source).c_str(), &file_second.value), VXCORE_OK);
  ASSERT_EQ(utf8_to_fs_path(file_first.value).filename(), fs::path("file.bin"));
  ASSERT_EQ(utf8_to_fs_path(file_second.value).filename(), fs::path("file_1.bin"));
  ASSERT_TRUE(ReadBytes(notebook_root / utf8_to_fs_path(file_first.value)) == source_bytes);
  ASSERT_TRUE(ReadBytes(notebook_root / utf8_to_fs_path(file_second.value)) == second);
  ASSERT_TRUE(ReadBytes(source) == second);
  ASSERT_FALSE(HasTemporarySibling(notebook_root / utf8_to_fs_path(raw_first.value)));
  ASSERT_FALSE(HasTemporarySibling(notebook_root / utf8_to_fs_path(file_first.value)));
  ASSERT_FALSE(HasTemporarySibling(notebook_root / utf8_to_fs_path(file_second.value)));
  ASSERT_TRUE(ReadBytes(fixture.NotePath()) == note_before);
  CoreString attachments;
  ASSERT_EQ(vxcore_buffer_list_attachments(fixture.context, fixture.buffer.value, &attachments.value), VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(attachments.value), nlohmann::json::array());
  ASSERT_EQ(vxcore_buffer_close(fixture.context, fixture.buffer.value), VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_close(fixture.context, fixture.notebook.value), VXCORE_OK);
  ASSERT_EQ(vxcore_prepare_shutdown(fixture.context), VXCORE_OK);
  return 0;
}

int test_posix_permissions_survive() {
  Fixture fixture("mode");
  ASSERT_EQ(fixture.Initialize(), 0);
  ASSERT_EQ(::chmod(fixture.NotePath().c_str(), 0751), 0);
  ASSERT_EQ(vxcore_buffer_set_content_raw(fixture.context, fixture.buffer.value, "mode", 4), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(fixture.context, fixture.buffer.value), VXCORE_OK);
  struct stat info{};
  ASSERT_EQ(::stat(fixture.NotePath().c_str(), &info), 0);
  ASSERT_EQ(info.st_mode & 07777, 0751);
  return 0;
}
}  // namespace

int main() {
  vxcore_set_test_mode(1);
  RUN_TEST(test_source_bytes_and_ids_survive_reopen);
  RUN_TEST(test_binary_vector_and_empty_publication);
  RUN_TEST(test_prepublication_error_keeps_old_bytes);
  RUN_TEST(test_readonly_failure_retains_dirty_buffer);
  RUN_TEST(test_links_and_nonregular_targets_are_rejected);
  RUN_TEST(test_asset_publication_preserves_bytes_and_names);
  RUN_TEST(test_posix_permissions_survive);
  return 0;
}
