#include <chrono>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "test_utils.h"
#include "vxcore/vxcore.h"

namespace {
namespace fs = std::filesystem;

struct TempLeaf {
  fs::path path;
  explicit TempLeaf(const char *name) {
    static unsigned int sequence = 0;
    const auto parent = utf8_to_fs_path(get_test_path("buffer_save_desktop"));
    fs::create_directories(parent);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    do {
      path = parent / (std::string(name) + "-" + std::to_string(stamp) + "-" +
                       std::to_string(++sequence));
    } while (!fs::create_directory(path));
  }
  ~TempLeaf() { std::error_code ec; fs::remove_all(path, ec); }
};

struct Fixture {
  TempLeaf leaf;
  VxCoreContextHandle context = nullptr;
  char *buffer = nullptr;
  explicit Fixture(const char *name) : leaf(name) {}
  ~Fixture() {
    vxcore_context_destroy(context);
    vxcore_string_free(buffer);
  }
  fs::path NotePath() const { return leaf.path / utf8_to_fs_path("笔记-é.md"); }
  int Initialize() {
    write_file(fs_path_to_utf8(NotePath()), "original source");
    // Use the legacy factory even when the optional mobile test API is enabled.
    ASSERT_EQ(vxcore_context_create(nullptr, &context), VXCORE_OK);
    ASSERT_EQ(vxcore_buffer_open(context, nullptr, fs_path_to_utf8(NotePath()).c_str(),
                                 &buffer), VXCORE_OK);
    return 0;
  }
};

std::vector<uint8_t> ReadBytes(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Unable to read fixture");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int SaveAndCheck(Fixture &fixture, const std::vector<uint8_t> &body) {
  ASSERT_EQ(vxcore_buffer_set_content_raw(fixture.context, fixture.buffer,
                                          body.data(), body.size()), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(fixture.context, fixture.buffer), VXCORE_OK);
  ASSERT_TRUE(ReadBytes(fixture.NotePath()) == body);
  int modified = 1;
  ASSERT_EQ(vxcore_buffer_is_modified(fixture.context, fixture.buffer, &modified), VXCORE_OK);
  ASSERT_EQ(modified, 0);
  ASSERT_EQ(vxcore_buffer_reload(fixture.context, fixture.buffer), VXCORE_OK);
  const void *reloaded = nullptr;
  size_t size = 0;
  ASSERT_EQ(vxcore_buffer_get_content_raw(fixture.context, fixture.buffer, &reloaded, &size),
            VXCORE_OK);
  ASSERT_EQ(size, body.size());
  ASSERT_TRUE(body.empty() || std::memcmp(reloaded, body.data(), size) == 0);
  return 0;
}

int test_hardlink_alias_receives_in_place_writes() {
  Fixture fixture("hardlink");
  ASSERT_EQ(fixture.Initialize(), 0);
  const auto alias = fixture.leaf.path / "hard-link";
  fs::create_hard_link(fixture.NotePath(), alias);
  ASSERT_TRUE(fs::equivalent(fixture.NotePath(), alias));
  const std::vector<uint8_t> body{'n', 'e', 'w', 0, 0xff, '\r', '\n'};
  ASSERT_EQ(SaveAndCheck(fixture, body), 0);
  ASSERT_TRUE(ReadBytes(alias) == body);
  ASSERT_TRUE(fs::equivalent(fixture.NotePath(), alias));
  ASSERT_EQ(fs::hard_link_count(fixture.NotePath()), 2);
  ASSERT_EQ(SaveAndCheck(fixture, {}), 0);
  ASSERT_TRUE(ReadBytes(alias).empty());
  ASSERT_TRUE(fs::equivalent(fixture.NotePath(), alias));
  return 0;
}

#ifdef _WIN32
struct HeldHandle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~HeldHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

int test_write_succeeds_when_open_handle_denies_delete() {
  Fixture fixture("held-handle");
  ASSERT_EQ(fixture.Initialize(), 0);
  HeldHandle held;
  held.value = CreateFileW(fixture.NotePath().c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  ASSERT_NE(held.value, INVALID_HANDLE_VALUE);
  // Prove the fixture denies replacement access, without deleting anything.
  HeldHandle deletion;
  deletion.value = CreateFileW(fixture.NotePath().c_str(), DELETE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  const DWORD deletion_error = GetLastError();
  ASSERT_EQ(deletion.value, INVALID_HANDLE_VALUE);
  ASSERT_EQ(deletion_error, ERROR_SHARING_VIOLATION);
  const std::vector<uint8_t> body{'w', 'r', 'i', 't', 'a', 'b', 'l', 'e'};
  ASSERT_EQ(SaveAndCheck(fixture, body), 0);
  // The held handle still observes the new bytes of the same file object.
  char observed[8]{};
  DWORD size = 0;
  ASSERT_TRUE(ReadFile(held.value, observed, sizeof(observed), &size, nullptr));
  ASSERT_EQ(size, body.size());
  ASSERT_TRUE(std::memcmp(observed, body.data(), body.size()) == 0);
  return 0;
}
#else
int test_symlink_preserved_and_target_updated() {
  Fixture fixture("symlink");
  ASSERT_EQ(fixture.Initialize(), 0);
  const auto target = fixture.leaf.path / "symlink-target";
  fs::rename(fixture.NotePath(), target);
  fs::create_symlink(target, fixture.NotePath());
  const auto link_value = fs::read_symlink(fixture.NotePath());
  const std::vector<uint8_t> body{'l', 'i', 'n', 'k', 'e', 'd'};
  ASSERT_EQ(SaveAndCheck(fixture, body), 0);
  ASSERT_TRUE(fs::is_symlink(fs::symlink_status(fixture.NotePath())));
  ASSERT_TRUE(fs::read_symlink(fixture.NotePath()) == link_value);
  ASSERT_TRUE(ReadBytes(target) == body);
  ASSERT_TRUE(fs::equivalent(fixture.NotePath(), target));
  return 0;
}
#endif
}  // namespace

int main() {
  vxcore_set_test_mode(1);
  RUN_TEST(test_hardlink_alias_receives_in_place_writes);
#ifdef _WIN32
  RUN_TEST(test_write_succeeds_when_open_handle_denies_delete);
#else
  RUN_TEST(test_symlink_preserved_and_target_updated);
#endif
  return 0;
}
