#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <git2.h>
#include <sodium.h>

#include "core/context.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"

// Simple base64 encoder for test purposes (RFC 4648)
static std::string test_base64_encode(const uint8_t *data, size_t size) {
  static const char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(((size + 2) / 3) * 4);

  for (size_t i = 0; i < size; i += 3) {
    uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < size) triple |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < size) triple |= static_cast<uint32_t>(data[i + 2]);

    result += chars[(triple >> 18) & 0x3F];
    result += chars[(triple >> 12) & 0x3F];
    result += (i + 1 < size) ? chars[(triple >> 6) & 0x3F] : '=';
    result += (i + 2 < size) ? chars[triple & 0x3F] : '=';
  }
  return result;
}

// Simple base64 decoder for test purposes
static std::vector<uint8_t> test_base64_decode(const std::string &encoded) {
  static const int8_t table[256] = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62,
      -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -2, -1, -1, -1, 0,
      1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
      23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
      39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
  };
  std::vector<uint8_t> result;
  uint32_t buffer = 0;
  int bits = 0;
  for (char c : encoded) {
    int8_t val = table[static_cast<uint8_t>(c)];
    if (val >= 0) {
      buffer = (buffer << 6) | static_cast<uint32_t>(val);
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        result.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
      }
    }
  }
  return result;
}

// Helper to create a test file
void create_test_file(const std::string &path, const std::string &content) {
  std::ofstream file(path, std::ios::binary);
  if (file.is_open()) {
    file.write(content.c_str(), content.length());
    file.close();
  }
}

// Helper to read file content
std::string read_file_content(const std::string &path) {
  std::ifstream file(utf8_to_fs_path(path), std::ios::binary | std::ios::ate);
  if (!file.is_open()) return "";

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::string buffer(size, '\0');
  file.read(&buffer[0], size);
  return buffer;
}

// Snapshot names, file kinds and bytes without following symlinks.
static nlohmann::json snapshot_attachment_test_tree(const std::string &path) {
  const auto root = utf8_to_fs_path(path);
  auto snapshot = nlohmann::json::object();
  for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
    const auto name = normalize_path(fs_path_to_utf8(entry.path().lexically_relative(root)));
    const auto status = entry.symlink_status();
    auto record = nlohmann::json::object();
    record["type"] = static_cast<int>(status.type());
    if (std::filesystem::is_regular_file(status)) {
      record["content"] = read_file_content(fs_path_to_utf8(entry.path()));
    } else if (std::filesystem::is_symlink(status)) {
      record["target"] = fs_path_to_utf8(std::filesystem::read_symlink(entry.path()));
    }
    snapshot[name] = std::move(record);
  }
  return snapshot;
}

namespace {

// Every process and encryption fixture owns a fresh temp leaf. Existing tests may
// clear their test config without deleting another ctest process's profile.
class BufferTestTempDirectory {
 public:
  BufferTestTempDirectory() {
    const auto parent = std::filesystem::temp_directory_path();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      path_ = parent / ("vxcore_buffer_" + std::to_string(stamp) + "_" +
                        std::to_string(attempt));
      std::error_code ec;
      if (std::filesystem::create_directory(path_, ec)) {
        owns_path_ = true;
        break;
      }
      if (ec) return;
    }
    if (owns_path_ == false) return;
#ifdef _WIN32
    const wchar_t *names[] = {L"TMP", L"TEMP", L"TMPDIR"};
    for (size_t i = 0; i < 3; ++i) {
      const wchar_t *value = _wgetenv(names[i]);
      saved_[i] = value ? value : L"";
    }
    changed_environment_ = true;
    valid_ = true;
    for (const auto *name : names) {
      if (_wputenv_s(name, path_.c_str()) != 0) valid_ = false;
    }
#else
    const char *names[] = {"TMP", "TEMP", "TMPDIR"};
    for (size_t i = 0; i < 3; ++i) {
      const char *value = std::getenv(names[i]);
      present_[i] = value != nullptr;
      saved_[i] = value ? value : "";
    }
    changed_environment_ = true;
    valid_ = true;
    for (const auto *name : names) {
      if (setenv(name, path_.c_str(), 1) != 0) valid_ = false;
    }
#endif
  }

  ~BufferTestTempDirectory() {
    if (changed_environment_) {
#ifdef _WIN32
      const wchar_t *names[] = {L"TMP", L"TEMP", L"TMPDIR"};
      for (size_t i = 0; i < 3; ++i) _wputenv_s(names[i], saved_[i].c_str());
#else
      const char *names[] = {"TMP", "TEMP", "TMPDIR"};
      for (size_t i = 0; i < 3; ++i) {
        if (present_[i]) setenv(names[i], saved_[i].c_str(), 1);
        else unsetenv(names[i]);
      }
#endif
    }
    if (owns_path_) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }

  bool valid() const { return valid_; }
  const std::filesystem::path &path() const { return path_; }

 private:
  std::filesystem::path path_;
#ifdef _WIN32
  std::wstring saved_[3];
#else
  std::string saved_[3];
  bool present_[3] = {};
#endif
  bool owns_path_ = false;
  bool changed_environment_ = false;
  bool valid_ = false;
};

struct EncryptionTestString {
  char *value = nullptr;
  ~EncryptionTestString() { vxcore_string_free(value); }
};

struct EncryptionTestContext {
  VxCoreContextHandle value = nullptr;
  ~EncryptionTestContext() { vxcore_context_destroy(value); }
};

struct EncryptionFixture {
  BufferTestTempDirectory directory;
  EncryptionTestContext context;
  EncryptionTestString notebook_id;
  std::string path;
  VxCoreError error = VXCORE_ERR_IO;

  EncryptionFixture() {
    if (directory.valid() == false) return;
    path = get_test_path("notebook");
    error = vxcore_context_create(nullptr, &context.value);
    if (error != VXCORE_OK) return;
    error = vxcore_notebook_create(context.value, path.c_str(), "{\"name\":\"Encryption Test\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id.value);
  }

  std::string key_path() const { return path + "/vx_notebook/encryption.vne"; }
};

const char kEncryptionPasswordBytes[] = "  PRIVATE_PASSWORD_SENTINEL_7e9d_\xC3\xA9\0suffix  ";
const std::string kEncryptionPassword(kEncryptionPasswordBytes,
                                      sizeof(kEncryptionPasswordBytes) - 1);

std::map<std::string, std::string> encryption_file_tree(const std::string &root) {
  std::map<std::string, std::string> result;
  const auto root_path = utf8_to_fs_path(root);
  for (const auto &entry : std::filesystem::recursive_directory_iterator(root_path)) {
    const auto relative = entry.path().lexically_relative(root_path).generic_u8string();
    if (entry.is_directory()) {
      result[relative + "/"] = "";
    } else {
      std::ifstream stream(entry.path(), std::ios::binary);
      result[relative] = std::string(std::istreambuf_iterator<char>(stream), {});
    }
  }
  return result;
}

nlohmann::json encryption_status(VxCoreContextHandle context, const char *notebook_id,
                                 const char *file_path = nullptr) {
  EncryptionTestString result;
  if (vxcore_encryption_get_status(context, notebook_id, file_path, &result.value) != VXCORE_OK ||
      result.value == nullptr) {
    return nullptr;
  }
  return nlohmann::json::parse(result.value, nullptr, false);
}

nlohmann::json expected_encryption_status(bool initialized, bool unlocked,
                                          const std::string &vault_id = "") {
  return {{"initialized", initialized}, {"unlocked", unlocked},
          {"encrypted", false}, {"vaultId", vault_id}};
}

VxCoreError initialize_test_encryption(VxCoreContextHandle context, const char *notebook_id,
                                       const char *source_notebook_id = nullptr) {
  VxCoreEncryptionSetupHandle setup = nullptr;
  auto error = vxcore_encryption_prepare_notebook(context, notebook_id, source_notebook_id,
                                                  kEncryptionPassword.data(),
                                                  kEncryptionPassword.size(), &setup);
  if (error != VXCORE_OK) return error;
  return vxcore_encryption_commit_notebook(context, setup);
}

std::string encryption_key_file(const std::string &json) {
  std::string result("VNEKEY1\0", 8);
  const auto size = static_cast<uint32_t>(json.size());
  for (unsigned shift = 0; shift < 32; shift += 8) {
    result.push_back(static_cast<char>((size >> shift) & 0xff));
  }
  result += json;
  return result;
}

int assert_key_envelope(const std::string &bytes, const char *notebook_id) {
  ASSERT(bytes.size() >= 12);
  ASSERT(bytes.size() <= 4096);
  ASSERT_EQ(bytes.substr(0, 8), std::string("VNEKEY1\0", 8));
  uint32_t json_size = 0;
  for (unsigned i = 0; i < 4; ++i) {
    json_size |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[8 + i])) << (i * 8);
  }
  ASSERT_EQ(bytes.size(), static_cast<size_t>(12) + json_size);
  bool duplicate = false;
  std::set<std::string> keys;
  auto json = nlohmann::json::parse(bytes.substr(12),
      [&](int, nlohmann::json::parse_event_t event, nlohmann::json &value) {
        if (event == nlohmann::json::parse_event_t::key) {
          if (keys.insert(value.get<std::string>()).second == false) duplicate = true;
        }
        return true;
      }, false);
  ASSERT_FALSE(json.is_discarded());
  ASSERT_FALSE(duplicate);
  const std::set<std::string> expected = {
      "version", "vaultId", "notebookId", "notebookKeyId", "kdf", "opslimit", "memlimit",
      "salt", "masterNonce", "wrappedMasterKey", "notebookNonce", "wrappedNotebookKey"};
  ASSERT(keys == expected);
  ASSERT_EQ(json["version"], 1);
  ASSERT_EQ(json["kdf"], "argon2id13");
  ASSERT_EQ(json["opslimit"], 3);
  ASSERT_EQ(json["memlimit"], 67108864);
  ASSERT_EQ(json["notebookId"], notebook_id);
  for (const auto *key : {"vaultId", "notebookId", "notebookKeyId"}) {
    const auto uuid = json.at(key).get<std::string>();
    ASSERT_EQ(uuid.size(), static_cast<size_t>(36));
    for (size_t i = 0; i < uuid.size(); ++i) {
      if (i == 8 || i == 13 || i == 18 || i == 23) ASSERT_EQ(uuid[i], '-');
      else ASSERT((uuid[i] >= '0' && uuid[i] <= '9') || (uuid[i] >= 'a' && uuid[i] <= 'f'));
    }
  }
  const std::pair<const char *, size_t> binary_fields[] = {
      {"salt", 16}, {"masterNonce", 24}, {"wrappedMasterKey", 48},
      {"notebookNonce", 24}, {"wrappedNotebookKey", 48}};
  for (const auto &field : binary_fields) {
    const auto encoded = json.at(field.first).get<std::string>();
    const auto decoded = test_base64_decode(encoded);
    ASSERT_EQ(decoded.size(), field.second);
    ASSERT_EQ(test_base64_encode(decoded.data(), decoded.size()), encoded);
  }
  ASSERT_EQ(bytes.find("PRIVATE_PASSWORD_SENTINEL_7e9d"), std::string::npos);
  ASSERT_EQ(bytes.find(test_base64_encode(
      reinterpret_cast<const uint8_t *>(kEncryptionPassword.data()), kEncryptionPassword.size())),
      std::string::npos);
  return 0;
}

struct EncryptionTestGitRepository {
  git_repository *value = nullptr;
  EncryptionTestGitRepository() { git_libgit2_init(); }
  ~EncryptionTestGitRepository() {
    git_repository_free(value);
    git_libgit2_shutdown();
  }
};

int assert_binary_encryption_attributes(git_repository *repository) {
  for (const auto *file : {"note.md.vne", "vx_notebook/encryption.vne",
                           "vx_assets/document/object.vne"}) {
    for (const auto *attribute : {"text", "diff", "merge"}) {
      const char *value = nullptr;
      ASSERT_EQ(git_attr_get(&value, repository, GIT_ATTR_CHECK_NO_SYSTEM, file, attribute), 0);
      ASSERT(GIT_ATTR_IS_FALSE(value));
    }
  }
  const char *eol = nullptr;
  ASSERT_EQ(git_attr_get(&eol, repository, GIT_ATTR_CHECK_NO_SYSTEM, "plain.md", "eol"), 0);
  ASSERT_NOT_NULL(eol);
  ASSERT_EQ(std::string(eol), "lf");
  return 0;
}

}  // namespace

int test_buffer_open_close() {
  std::cout << "  Running test_buffer_open_close..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_open"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_open").c_str(),
                               "{\"name\":\"Buffer Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a test file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id);

  // Close buffer
  err = vxcore_buffer_close(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_open"));
  std::cout << "  ✓ test_buffer_open_close passed" << std::endl;
  return 0;
}

int test_buffer_get() {
  std::cout << "  Running test_buffer_get..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_get"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_get").c_str(),
                               "{\"name\":\"Buffer Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Get buffer info
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_json);

  // Verify JSON contains expected fields
  nlohmann::json buffer_data = nlohmann::json::parse(buffer_json);
  ASSERT(buffer_data.contains("id"));
  ASSERT(buffer_data.contains("filePath"));
  ASSERT(buffer_data["filePath"] == "test.md");

  vxcore_string_free(buffer_json);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_get"));
  std::cout << "  ✓ test_buffer_get passed" << std::endl;
  return 0;
}

int test_buffer_list() {
  std::cout << "  Running test_buffer_list..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_list"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_list").c_str(),
                               "{\"name\":\"Buffer Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id1 = nullptr, *file_id2 = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test1.md", &file_id1);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_file_create(ctx, notebook_id, ".", "test2.md", &file_id2);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id1 = nullptr, *buffer_id2 = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test1.md", &buffer_id1);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_open(ctx, notebook_id, "test2.md", &buffer_id2);
  ASSERT_EQ(err, VXCORE_OK);

  // List buffers
  char *list_json = nullptr;
  err = vxcore_buffer_list(ctx, &list_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(list_json);

  nlohmann::json buffer_list = nlohmann::json::parse(list_json);
  ASSERT(buffer_list.is_array());
  // Note: May have buffers from previous tests due to session persistence
  // We just check that our two buffers are present
  bool found_buffer1 = false, found_buffer2 = false;
  for (const auto &buf : buffer_list) {
    std::string id = buf["id"];
    if (id == buffer_id1) found_buffer1 = true;
    if (id == buffer_id2) found_buffer2 = true;
  }
  ASSERT(found_buffer1);
  ASSERT(found_buffer2);

  vxcore_string_free(list_json);
  vxcore_string_free(buffer_id1);
  vxcore_string_free(buffer_id2);
  vxcore_string_free(file_id1);
  vxcore_string_free(file_id2);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_list"));
  std::cout << "  ✓ test_buffer_list passed" << std::endl;
  return 0;
}

int test_buffer_content_raw() {
  std::cout << "  Running test_buffer_content_raw..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_content_raw"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_content_raw").c_str(),
                               "{\"name\":\"Buffer Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Set content using raw API
  const char *test_content = "Hello, VxCore!";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, test_content, strlen(test_content));
  ASSERT_EQ(err, VXCORE_OK);

  // Get content using raw API
  const void *content_ptr = nullptr;
  size_t content_size = 0;
  err = vxcore_buffer_get_content_raw(ctx, buffer_id, &content_ptr, &content_size);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(content_ptr);
  ASSERT_EQ(content_size, strlen(test_content));

  std::string retrieved(static_cast<const char *>(content_ptr), content_size);
  ASSERT_EQ(retrieved, std::string(test_content));

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_content_raw"));
  std::cout << "  ✓ test_buffer_content_raw passed" << std::endl;
  return 0;
}

int test_buffer_content_json() {
  std::cout << "  Running test_buffer_content_json..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_content_json"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_content_json").c_str(),
                               "{\"name\":\"Buffer Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Set content using JSON API (base64-encoded)
  const char *test_content = "JSON Content Test";
  std::string base64_content =
      test_base64_encode(reinterpret_cast<const uint8_t *>(test_content), strlen(test_content));

  nlohmann::json content_json;
  content_json["content"] = base64_content;
  std::string content_json_str = content_json.dump();

  err = vxcore_buffer_set_content(ctx, buffer_id, content_json_str.c_str());
  ASSERT_EQ(err, VXCORE_OK);

  // Get content using JSON API
  char *retrieved_json = nullptr;
  err = vxcore_buffer_get_content(ctx, buffer_id, &retrieved_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(retrieved_json);

  nlohmann::json retrieved_data = nlohmann::json::parse(retrieved_json);
  ASSERT(retrieved_data.contains("content"));
  ASSERT_EQ(retrieved_data["content"].get<std::string>(), base64_content);

  // Verify decoding back to original content
  std::vector<uint8_t> decoded = test_base64_decode(retrieved_data["content"].get<std::string>());
  std::string decoded_str(decoded.begin(), decoded.end());
  ASSERT_EQ(decoded_str, std::string(test_content));

  vxcore_string_free(retrieved_json);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_content_json"));
  std::cout << "  ✓ test_buffer_content_json passed" << std::endl;
  return 0;
}

int test_buffer_save_reload() {
  std::cout << "  Running test_buffer_save_reload..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_save_reload"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_save_reload");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Buffer Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Set content
  const char *original_content = "Original content";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, original_content, strlen(original_content));
  ASSERT_EQ(err, VXCORE_OK);

  // Save to disk
  err = vxcore_buffer_save(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify file on disk
  std::string file_path = notebook_path + "/test.md";
  std::string disk_content = read_file_content(file_path);
  ASSERT_EQ(disk_content, std::string(original_content));

  // Get revision after save
  int revision_before_reload = 0;
  err = vxcore_buffer_get_revision(ctx, buffer_id, &revision_before_reload);
  ASSERT_EQ(err, VXCORE_OK);

  // Modify file on disk externally
  const char *modified_content = "Modified externally";
  create_test_file(file_path, modified_content);

  // Reload buffer
  err = vxcore_buffer_reload(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify revision incremented after reload
  int revision_after_reload = 0;
  err = vxcore_buffer_get_revision(ctx, buffer_id, &revision_after_reload);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(revision_after_reload, revision_before_reload + 1);

  // Verify reloaded content
  const void *content_ptr = nullptr;
  size_t content_size = 0;
  err = vxcore_buffer_get_content_raw(ctx, buffer_id, &content_ptr, &content_size);
  ASSERT_EQ(err, VXCORE_OK);

  std::string reloaded(static_cast<const char *>(content_ptr), content_size);
  ASSERT_EQ(reloaded, std::string(modified_content));

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_save_reload"));
  std::cout << "  ✓ test_buffer_save_reload passed" << std::endl;
  return 0;
}

int test_buffer_deduplication() {
  std::cout << "  Running test_buffer_deduplication..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_dedup"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_dedup").c_str(),
                               "{\"name\":\"Buffer Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer first time
  char *buffer_id1 = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id1);
  ASSERT_EQ(err, VXCORE_OK);

  // Open same buffer again - should return same ID
  char *buffer_id2 = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id2);
  ASSERT_EQ(err, VXCORE_OK);

  ASSERT_EQ(std::string(buffer_id1), std::string(buffer_id2));

  vxcore_string_free(buffer_id1);
  vxcore_string_free(buffer_id2);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_dedup"));
  std::cout << "  ✓ test_buffer_deduplication passed" << std::endl;
  return 0;
}

int test_buffer_external_file() {
  std::cout << "  Running test_buffer_external_file..." << std::endl;

  // Create external file outside any notebook
  std::string external_path = get_test_path("external_test_file.txt");
  create_test_file(external_path, "External file content");

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Open external file (notebook_id = NULL)
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, nullptr, external_path.c_str(), &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id);

  // Verify content
  const void *content_ptr = nullptr;
  size_t content_size = 0;
  err = vxcore_buffer_get_content_raw(ctx, buffer_id, &content_ptr, &content_size);
  ASSERT_EQ(err, VXCORE_OK);

  std::string content(static_cast<const char *>(content_ptr), content_size);
  ASSERT_EQ(content, "External file content");

  vxcore_string_free(buffer_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(external_path);
  std::cout << "  ✓ test_buffer_external_file passed" << std::endl;
  return 0;
}

int test_buffer_state() {
  std::cout << "  Running test_buffer_state..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_state"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_state");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Buffer Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Write initial content to disk
  std::string file_path = notebook_path + "/test.md";
  create_test_file(file_path, "Initial content");

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Initial state should be NORMAL
  VxCoreBufferState state;
  err = vxcore_buffer_get_state(ctx, buffer_id, &state);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_NORMAL);

  // Modify file externally and wait a bit to ensure timestamp changes
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  create_test_file(file_path, "Externally modified");

  // Check for external changes (state should change to FILE_CHANGED)
  err = vxcore_buffer_reload(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_state"));
  std::cout << "  ✓ test_buffer_state passed" << std::endl;
  return 0;
}

int test_buffer_is_modified() {
  std::cout << "  Running test_buffer_is_modified..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_modified"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_modified");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Modified Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Write initial content to disk
  std::string file_path = notebook_path + "/test.md";
  create_test_file(file_path, "Initial content");

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Load content first (triggers lazy loading)
  const void *content_ptr = nullptr;
  size_t content_size = 0;
  err = vxcore_buffer_get_content_raw(ctx, buffer_id, &content_ptr, &content_size);
  ASSERT_EQ(err, VXCORE_OK);

  // Initially should not be modified
  int is_modified = -1;
  err = vxcore_buffer_is_modified(ctx, buffer_id, &is_modified);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(is_modified, 0);

  // Modify content
  const char *new_content = "Modified content";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, new_content, strlen(new_content));
  ASSERT_EQ(err, VXCORE_OK);

  // Should now be modified
  err = vxcore_buffer_is_modified(ctx, buffer_id, &is_modified);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(is_modified, 1);

  // Save buffer
  err = vxcore_buffer_save(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // After save, should not be modified
  err = vxcore_buffer_is_modified(ctx, buffer_id, &is_modified);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(is_modified, 0);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_modified"));
  std::cout << "  ✓ test_buffer_is_modified passed" << std::endl;
  return 0;
}

int test_buffer_get_revision() {
  std::cout << "  Running test_buffer_get_revision..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_revision"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_revision");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Revision Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Write initial content to disk
  std::string file_path = notebook_path + "/test.md";
  create_test_file(file_path, "Initial content");

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Get initial revision (before content loaded)
  int revision = -1;
  err = vxcore_buffer_get_revision(ctx, buffer_id, &revision);
  ASSERT_EQ(err, VXCORE_OK);
  int initial_rev = revision;

  // Load content (triggers lazy loading)
  const void *content_ptr = nullptr;
  size_t content_size = 0;
  err = vxcore_buffer_get_content_raw(ctx, buffer_id, &content_ptr, &content_size);
  ASSERT_EQ(err, VXCORE_OK);

  // Set content — revision should increment
  const char *new_content = "Modified content";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, new_content, strlen(new_content));
  ASSERT_EQ(err, VXCORE_OK);

  int after_set_rev = -1;
  err = vxcore_buffer_get_revision(ctx, buffer_id, &after_set_rev);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_TRUE(after_set_rev > initial_rev);

  // Save — revision should increment again
  err = vxcore_buffer_save(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  int after_save_rev = -1;
  err = vxcore_buffer_get_revision(ctx, buffer_id, &after_save_rev);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_TRUE(after_save_rev > after_set_rev);

  // Verify get_revision matches the JSON "revision" field from buffer_get
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto json = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(json["revision"].get<int>(), after_save_rev);
  vxcore_string_free(buffer_json);

  // Error cases
  int dummy = -1;
  err = vxcore_buffer_get_revision(ctx, "nonexistent_id", &dummy);
  ASSERT_EQ(err, VXCORE_ERR_BUFFER_NOT_FOUND);

  err = vxcore_buffer_get_revision(nullptr, buffer_id, &dummy);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  err = vxcore_buffer_get_revision(ctx, buffer_id, nullptr);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_revision"));
  std::cout << "  ✓ test_buffer_get_revision passed" << std::endl;
  return 0;
}

int test_buffer_write_backup() {
  std::cout << "  Running test_buffer_write_backup..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_write_backup"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_write_backup");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Backup Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  const char *content = "backup test content";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, content, strlen(content));
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_buffer_write_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string backup_path = notebook_path + "/test.md.vswp";
  ASSERT_TRUE(std::filesystem::exists(backup_path));

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_write_backup"));
  std::cout << "  ✓ test_buffer_write_backup passed" << std::endl;
  return 0;
}

int test_buffer_has_backup() {
  std::cout << "  Running test_buffer_has_backup..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_has_backup"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_has_backup").c_str(),
                               "{\"name\":\"Backup Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  const char *content = "has backup content";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, content, strlen(content));
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_buffer_write_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  int has_backup = 0;
  err = vxcore_buffer_has_backup(ctx, buffer_id, &has_backup);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(has_backup, 1);

  err = vxcore_buffer_discard_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  has_backup = 1;
  err = vxcore_buffer_has_backup(ctx, buffer_id, &has_backup);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(has_backup, 0);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_has_backup"));
  std::cout << "  ✓ test_buffer_has_backup passed" << std::endl;
  return 0;
}

int test_buffer_recover_backup() {
  std::cout << "  Running test_buffer_recover_backup..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_recover_backup"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_recover_backup");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Backup Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  const char *original = "original";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, original, strlen(original));
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_save(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  const char *backup_content = "modified for backup";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, backup_content, strlen(backup_content));
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_write_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  const char *other_content = "some other content";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, other_content, strlen(other_content));
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_buffer_recover_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string file_path = notebook_path + "/test.md";
  std::string disk_content = read_file_content(file_path);
  ASSERT_EQ(disk_content, std::string(backup_content));

  std::string backup_path = file_path + ".vswp";
  ASSERT_FALSE(std::filesystem::exists(backup_path));

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_recover_backup"));
  std::cout << "  ✓ test_buffer_recover_backup passed" << std::endl;
  return 0;
}

int test_buffer_discard_backup() {
  std::cout << "  Running test_buffer_discard_backup..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_discard_backup"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_discard_backup").c_str(),
                               "{\"name\":\"Backup Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  const char *content = "discard backup content";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, content, strlen(content));
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_buffer_write_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  int has_backup = 0;
  err = vxcore_buffer_has_backup(ctx, buffer_id, &has_backup);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(has_backup, 1);

  err = vxcore_buffer_discard_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  has_backup = 1;
  err = vxcore_buffer_has_backup(ctx, buffer_id, &has_backup);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(has_backup, 0);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_discard_backup"));
  std::cout << "  ✓ test_buffer_discard_backup passed" << std::endl;
  return 0;
}

int test_buffer_get_backup_path() {
  std::cout << "  Running test_buffer_get_backup_path..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_get_backup_path"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_get_backup_path");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Backup Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *backup_path = nullptr;
  err = vxcore_buffer_get_backup_path(ctx, buffer_id, &backup_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(backup_path);

  std::string backup_path_str = normalize_path(backup_path);
  std::string notebook_path_str = normalize_path(notebook_path);
  ASSERT_TRUE(backup_path_str.size() >= 5 &&
              backup_path_str.substr(backup_path_str.size() - 5) == ".vswp");
  ASSERT_TRUE(backup_path_str.rfind(notebook_path_str, 0) == 0);

  vxcore_string_free(backup_path);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_get_backup_path"));
  std::cout << "  ✓ test_buffer_get_backup_path passed" << std::endl;
  return 0;
}

// Regression for issue #2721: a note whose path contains non-ASCII characters
// (CJK + full-width parentheses（）) must resolve/write its .vswp backup path
// without throwing "No mapping for the Unicode character exists in the target
// multi-byte code page". The bug was CleanFsPath(std::string) in
// Buffer::GetBackupFilePath, which does an ANSI-codepage narrow->path
// conversion; the throw propagated out of the save/move path and surfaced as a
// spurious "Failed to move" error that also left assets behind.
int test_buffer_backup_path_non_ascii() {
  std::cout << "  Running test_buffer_backup_path_non_ascii..." << std::endl;
  // Notebook root under a CJK + full-width（）subfolder.
  std::string base = get_test_path("test_buffer_backup_non_ascii");
  cleanup_test_dir(base);
  // "3个月临时（自动清空）" as UTF-8.
  std::string notebook_path =
      base + "/3\xe4\xb8\xaa\xe6\x9c\x88\xe4\xb8\xb4\xe6\x97\xb6\xef\xbc\x88\xe8\x87\xaa\xe5\x8a\xa8\xe6\xb8\x85\xe7\xa9\xba\xef\xbc\x89";

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"cn\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  // Note name "笔记.md".
  const char *note = "\xe7\xac\x94\xe8\xae\xb0.md";
  err = vxcore_file_create(ctx, notebook_id, ".", note, &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, note, &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Must NOT throw / error out on the non-ASCII path.
  char *backup_path = nullptr;
  err = vxcore_buffer_get_backup_path(ctx, buffer_id, &backup_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(backup_path);
  std::string bp = normalize_path(backup_path);
  ASSERT_TRUE(bp.size() >= 5 && bp.substr(bp.size() - 5) == ".vswp");
  // Exact expected path (locale-independent value check): <root>/<note>.vswp.
  std::string expected = normalize_path(notebook_path + "/" + note + ".vswp");
  ASSERT_TRUE(bp == expected);
  std::string backup_path_str = backup_path;
  vxcore_string_free(backup_path);

  // Set content then write the backup: exercises WriteBackup -> GetBackupFilePath
  // on the non-ASCII path too, and confirm the file physically lands at the
  // returned path.
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, "# hi\n", 5);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_write_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_TRUE(path_exists(backup_path_str));

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(base);
  std::cout << "  ✓ test_buffer_backup_path_non_ascii passed" << std::endl;
  return 0;
}

int test_buffer_backup_no_content() {
  std::cout << "  Running test_buffer_backup_no_content..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_backup_no_content"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_backup_no_content").c_str(),
                               "{\"name\":\"Backup Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_buffer_write_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_STATE);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_backup_no_content"));
  std::cout << "  ✓ test_buffer_backup_no_content passed" << std::endl;
  return 0;
}

int test_buffer_recover_no_backup() {
  std::cout << "  Running test_buffer_recover_no_backup..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_recover_no_backup"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_recover_no_backup").c_str(),
                               "{\"name\":\"Backup Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_buffer_recover_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_recover_no_backup"));
  std::cout << "  ✓ test_buffer_recover_no_backup passed" << std::endl;
  return 0;
}

int test_buffer_discard_no_backup() {
  std::cout << "  Running test_buffer_discard_no_backup..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_discard_no_backup"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_discard_no_backup").c_str(),
                               "{\"name\":\"Backup Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_buffer_discard_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_discard_no_backup"));
  std::cout << "  ✓ test_buffer_discard_no_backup passed" << std::endl;
  return 0;
}

int test_buffer_backup_format() {
  std::cout << "  Running test_buffer_backup_format..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_backup_format"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_backup_format").c_str(),
                               "{\"name\":\"Backup Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  const char *content = "format test content";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, content, strlen(content));
  ASSERT_EQ(err, VXCORE_OK);

  err = vxcore_buffer_write_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *backup_path = nullptr;
  err = vxcore_buffer_get_backup_path(ctx, buffer_id, &backup_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(backup_path);

  std::string backup_raw = read_file_content(backup_path);
  ASSERT_TRUE(backup_raw.rfind("vnotex_backup_file ", 0) == 0);
  size_t sep_pos = backup_raw.find('|');
  ASSERT_NE(sep_pos, std::string::npos);
  ASSERT_EQ(backup_raw.substr(sep_pos + 1), std::string(content));

  vxcore_string_free(backup_path);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_backup_format"));
  std::cout << "  ✓ test_buffer_backup_format passed" << std::endl;
  return 0;
}

int test_buffer_backup_overwrite() {
  std::cout << "  Running test_buffer_backup_overwrite..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_backup_overwrite"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_backup_overwrite");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Backup Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  const char *first_content = "first backup";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, first_content, strlen(first_content));
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_write_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  const char *second_content = "second backup";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, second_content, strlen(second_content));
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_write_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *backup_path = nullptr;
  err = vxcore_buffer_get_backup_path(ctx, buffer_id, &backup_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(backup_path);

  std::string backup_raw = read_file_content(backup_path);
  size_t sep_pos = backup_raw.find('|');
  ASSERT_NE(sep_pos, std::string::npos);
  ASSERT_EQ(backup_raw.substr(sep_pos + 1), std::string(second_content));

  size_t vswp_count = 0;
  for (const auto &entry : std::filesystem::directory_iterator(notebook_path)) {
    if (entry.is_regular_file() && entry.path().extension() == ".vswp") {
      ++vswp_count;
    }
  }
  ASSERT_EQ(vswp_count, 1u);

  vxcore_string_free(backup_path);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_backup_overwrite"));
  std::cout << "  ✓ test_buffer_backup_overwrite passed" << std::endl;
  return 0;
}

int test_buffer_notebook_close() {
  std::cout << "  Running test_buffer_notebook_close..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_nb_close"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_nb_close").c_str(),
                               "{\"name\":\"Buffer Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Close notebook - should close all buffers
  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Try to get closed buffer - should fail
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_ERR_BUFFER_NOT_FOUND);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_nb_close"));
  std::cout << "  ✓ test_buffer_notebook_close passed" << std::endl;
  return 0;
}

int test_buffer_persistence() {
  std::cout << "  Running test_buffer_persistence..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_persist"));

  std::string notebook_id_str;
  std::string buffer_id_str;

  // Create notebook, file, and buffer
  {
    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    ASSERT_EQ(err, VXCORE_OK);

    char *notebook_id = nullptr;
    err =
        vxcore_notebook_create(ctx, get_test_path("test_buffer_persist").c_str(),
                               "{\"name\":\"Buffer Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
    ASSERT_EQ(err, VXCORE_OK);
    notebook_id_str = std::string(notebook_id);

    char *file_id = nullptr;
    err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
    ASSERT_EQ(err, VXCORE_OK);

    char *buffer_id = nullptr;
    err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
    ASSERT_EQ(err, VXCORE_OK);
    buffer_id_str = std::string(buffer_id);

    vxcore_string_free(buffer_id);
    vxcore_string_free(file_id);
    vxcore_string_free(notebook_id);
    vxcore_context_destroy(ctx);  // Should save session config
  }

  // Reload and verify buffer persisted with same ID
  {
    VxCoreContextHandle ctx = nullptr;
    VxCoreError err = vxcore_context_create(nullptr, &ctx);
    ASSERT_EQ(err, VXCORE_OK);

    // Notebook is auto-loaded from session config during context creation.
    // Buffer ID should be preserved across sessions.
    char *buffer_json = nullptr;
    err = vxcore_buffer_get(ctx, buffer_id_str.c_str(), &buffer_json);
    ASSERT_EQ(err, VXCORE_OK);

    nlohmann::json buffer_data = nlohmann::json::parse(buffer_json);
    ASSERT(buffer_data.contains("id"));
    ASSERT_EQ(buffer_data["id"].get<std::string>(), buffer_id_str);
    ASSERT(buffer_data.contains("filePath"));
    ASSERT_EQ(buffer_data["filePath"].get<std::string>(), "test.md");
    ASSERT(buffer_data.contains("notebookId"));
    ASSERT_EQ(buffer_data["notebookId"].get<std::string>(), notebook_id_str);

    vxcore_string_free(buffer_json);
    vxcore_context_destroy(ctx);
  }

  cleanup_test_dir(get_test_path("test_buffer_persist"));
  std::cout << "  ✓ test_buffer_persistence passed" << std::endl;
  return 0;
}

int test_buffer_lazy_loading() {
  std::cout << "  Running test_buffer_lazy_loading..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_lazy"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_lazy");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Lazy Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "lazy.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Write content to the file on disk
  std::string file_path = notebook_path + "/lazy.md";
  const char *initial_content = "Lazy loaded content";
  create_test_file(file_path, initial_content);

  // Open buffer - content should NOT be loaded yet
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "lazy.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Check buffer info - contentLoaded should be false
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json buffer_data = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buffer_data["contentLoaded"].get<bool>(), false);
  vxcore_string_free(buffer_json);

  // Get content - should trigger lazy loading
  const void *content_ptr = nullptr;
  size_t content_size = 0;
  err = vxcore_buffer_get_content_raw(ctx, buffer_id, &content_ptr, &content_size);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(content_ptr);
  ASSERT_EQ(content_size, strlen(initial_content));

  std::string loaded_content(static_cast<const char *>(content_ptr), content_size);
  ASSERT_EQ(loaded_content, std::string(initial_content));

  // Check buffer info again - contentLoaded should now be true
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  buffer_data = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buffer_data["contentLoaded"].get<bool>(), true);
  vxcore_string_free(buffer_json);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_lazy"));
  std::cout << "  ✓ test_buffer_lazy_loading passed" << std::endl;
  return 0;
}

// ============ Buffer Asset Tests (Filesystem Only) ============

int test_buffer_insert_asset_raw() {
  std::cout << "  Running test_buffer_insert_asset_raw..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_insert_asset_raw"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_insert_asset_raw").c_str(),
                               "{\"name\":\"Asset Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a test file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Insert asset with raw binary data (does NOT add to attachment list)
  const uint8_t image_data[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  char *relative_path = nullptr;
  err = vxcore_buffer_insert_asset_raw(ctx, buffer_id, "screenshot.png", image_data,
                                       sizeof(image_data), &relative_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(relative_path);

  // Verify relative path format (should contain vx_assets and file uuid)
  std::string rel_path(relative_path);
  ASSERT_TRUE(rel_path.find("vx_assets") != std::string::npos);
  ASSERT_TRUE(rel_path.find("screenshot.png") != std::string::npos);

  // Verify file was created on disk
  std::string notebook_root = get_test_path("test_buffer_insert_asset_raw");
  std::string asset_abs_path = notebook_root + "/" + rel_path;
  ASSERT_TRUE(path_exists(asset_abs_path));

  // Verify NOT added to attachment list (insert_asset_raw doesn't touch metadata)
  char *attachments_json = nullptr;
  err = vxcore_buffer_list_attachments(ctx, buffer_id, &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto json = nlohmann::json::parse(attachments_json);
  ASSERT_EQ(json.size(), 0u);  // Should be empty
  vxcore_string_free(attachments_json);

  vxcore_string_free(relative_path);
  vxcore_string_free(file_id);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_insert_asset_raw"));
  std::cout << "  ✓ test_buffer_insert_asset_raw passed" << std::endl;
  return 0;
}

int test_buffer_insert_asset_raw_raw_notebook() {
  std::cout << "  Running test_buffer_insert_asset_raw_raw_notebook..." << std::endl;
  cleanup_test_dir(get_test_path("test_buf_asset_raw_rawnb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buf_asset_raw_rawnb").c_str(),
                               "{\"name\":\"Raw Asset Test\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "note.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  const uint8_t image_data[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  char *relative_path = nullptr;
  err = vxcore_buffer_insert_asset_raw(ctx, buffer_id, "screenshot.png", image_data,
                                       sizeof(image_data), &relative_path);
  ASSERT_EQ(err, VXCORE_OK);  // was VXCORE_ERR_UNSUPPORTED before the fix
  ASSERT_NOT_NULL(relative_path);

  std::string rel_path(relative_path);
  ASSERT_TRUE(rel_path.find("vx_assets") != std::string::npos);
  ASSERT_TRUE(rel_path.find("screenshot.png") != std::string::npos);
  ASSERT_TRUE(path_exists(get_test_path("test_buf_asset_raw_rawnb") + "/" + rel_path));

  vxcore_string_free(relative_path);
  vxcore_string_free(file_id);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buf_asset_raw_rawnb"));
  std::cout << "  ✓ test_buffer_insert_asset_raw_raw_notebook passed" << std::endl;
  return 0;
}

int test_buffer_attachments_raw_notebook_unsupported() {
  std::cout << "  Running test_buffer_attachments_raw_notebook_unsupported..." << std::endl;
  cleanup_test_dir(get_test_path("test_buf_attach_rawnb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buf_attach_rawnb").c_str(),
                               "{\"name\":\"Raw Attach Test\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "note.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // insert_attachment: rejected, no output.
  std::string src = get_test_path("test_buf_attach_rawnb") + "/source.bin";
  write_file(src, "data");
  char *out_filename = nullptr;
  err = vxcore_buffer_insert_attachment(ctx, buffer_id, src.c_str(), &out_filename);
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);
  ASSERT_NULL(out_filename);

  // get_attachments_folder: rejected.
  char *folder = nullptr;
  err = vxcore_buffer_get_attachments_folder(ctx, buffer_id, &folder);
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);
  ASSERT_NULL(folder);

  // list_attachments: rejected.
  char *list_json = nullptr;
  err = vxcore_buffer_list_attachments(ctx, buffer_id, &list_json);
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);
  ASSERT_NULL(list_json);

  const auto original_tree = snapshot_attachment_test_tree(get_test_path("test_buf_attach_rawnb"));
  char sentinel = '\0';
  list_json = &sentinel;
  err = vxcore_buffer_list_unindexed_attachments(ctx, buffer_id, &list_json);
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);
  ASSERT_NULL(list_json);
  ASSERT_EQ(snapshot_attachment_test_tree(get_test_path("test_buf_attach_rawnb")), original_tree);

  // Sentinel file: the guard rejects BEFORE any path computation, so delete/rename
  // must not mutate ANY nearby filesystem state. (The real per-file target would be
  // <notebook>/vx_assets/<file-uuid>/..., but the guard short-circuits first, so any
  // sentinel location suffices to prove "no side effect after rejection".)
  std::string assets_dir = get_test_path("test_buf_attach_rawnb") + "/vx_assets";
  create_directory(assets_dir);
  std::string staged = assets_dir + "/existing.png";
  write_file(staged, "img");
  ASSERT_TRUE(path_exists(staged));

  const auto populated_tree = snapshot_attachment_test_tree(get_test_path("test_buf_attach_rawnb"));
  list_json = &sentinel;
  err = vxcore_buffer_list_unindexed_attachments(ctx, buffer_id, &list_json);
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);
  ASSERT_NULL(list_json);
  ASSERT_EQ(snapshot_attachment_test_tree(get_test_path("test_buf_attach_rawnb")), populated_tree);

  // delete_attachment: rejected, file untouched.
  err = vxcore_buffer_delete_attachment(ctx, buffer_id, "existing.png");
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);
  ASSERT_TRUE(path_exists(staged));

  // rename_attachment: rejected, original untouched, no renamed file created.
  char *new_name = nullptr;
  err = vxcore_buffer_rename_attachment(ctx, buffer_id, "existing.png", "renamed.png", &new_name);
  ASSERT_EQ(err, VXCORE_ERR_UNSUPPORTED);
  ASSERT_NULL(new_name);
  ASSERT_TRUE(path_exists(staged));
  ASSERT_FALSE(path_exists(assets_dir + "/renamed.png"));

  vxcore_string_free(file_id);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buf_attach_rawnb"));
  std::cout << "  ✓ test_buffer_attachments_raw_notebook_unsupported passed" << std::endl;
  return 0;
}

int test_buffer_asset_surface_raw_notebook() {
  std::cout << "  Running test_buffer_asset_surface_raw_notebook..." << std::endl;
  cleanup_test_dir(get_test_path("test_buf_asset_surface_rawnb"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buf_asset_surface_rawnb").c_str(),
                               "{\"name\":\"Raw Asset Surface\"}", VXCORE_NOTEBOOK_RAW, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "note.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // get_assets_folder resolves + creates the folder for raw.
  char *assets_folder = nullptr;
  err = vxcore_buffer_get_assets_folder(ctx, buffer_id, &assets_folder);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(assets_folder);
  ASSERT_TRUE(std::string(assets_folder).find("vx_assets") != std::string::npos);

  // insert_asset (copy an existing file) works for raw.
  std::string src = get_test_path("test_buf_asset_surface_rawnb") + "/pic.png";
  write_file(src, "imgbytes");
  char *rel = nullptr;
  err = vxcore_buffer_insert_asset(ctx, buffer_id, src.c_str(), &rel);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(rel);
  ASSERT_TRUE(std::string(rel).find("vx_assets") != std::string::npos);
  ASSERT_TRUE(path_exists(get_test_path("test_buf_asset_surface_rawnb") + "/" + std::string(rel)));

  vxcore_string_free(rel);
  vxcore_string_free(assets_folder);
  vxcore_string_free(file_id);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buf_asset_surface_rawnb"));
  std::cout << "  ✓ test_buffer_asset_surface_raw_notebook passed" << std::endl;
  return 0;
}

int test_buffer_insert_asset() {
  std::cout << "  Running test_buffer_insert_asset..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_insert_asset"));
  create_directory(get_test_path("test_buffer_insert_asset"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_insert_asset").c_str(),
                               "{\"name\":\"Asset Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a test file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a source file to copy
  std::string source_path = get_test_path("test_buffer_insert_asset") + "/source_image.png";
  const uint8_t image_data[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::ofstream ofs(source_path, std::ios::binary);
  ofs.write(reinterpret_cast<const char *>(image_data), sizeof(image_data));
  ofs.close();

  // Insert asset by copying file (does NOT add to attachment list)
  char *relative_path = nullptr;
  err = vxcore_buffer_insert_asset(ctx, buffer_id, source_path.c_str(), &relative_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(relative_path);

  // Verify relative path format
  std::string rel_path(relative_path);
  ASSERT_TRUE(rel_path.find("vx_assets") != std::string::npos);
  ASSERT_TRUE(rel_path.find("source_image.png") != std::string::npos);

  // Verify file was created on disk
  std::string notebook_root = get_test_path("test_buffer_insert_asset");
  std::string asset_abs_path = notebook_root + "/" + rel_path;
  ASSERT_TRUE(path_exists(asset_abs_path));

  vxcore_string_free(relative_path);
  vxcore_string_free(file_id);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_insert_asset"));
  std::cout << "  ✓ test_buffer_insert_asset passed" << std::endl;
  return 0;
}

int test_buffer_delete_asset() {
  std::cout << "  Running test_buffer_delete_asset..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_delete_asset"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_delete_asset").c_str(),
                               "{\"name\":\"Asset Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a test file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Insert asset (raw - doesn't touch metadata)
  const uint8_t data[] = {0x89, 'P', 'N', 'G'};
  char *relative_path = nullptr;
  err = vxcore_buffer_insert_asset_raw(ctx, buffer_id, "to_delete.png", data, sizeof(data),
                                       &relative_path);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify file exists
  std::string notebook_root = get_test_path("test_buffer_delete_asset");
  std::string asset_abs_path = notebook_root + "/" + relative_path;
  ASSERT_TRUE(path_exists(asset_abs_path));

  // Delete asset (doesn't touch metadata)
  err = vxcore_buffer_delete_asset(ctx, buffer_id, relative_path);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify file is gone
  ASSERT_FALSE(path_exists(asset_abs_path));

  vxcore_string_free(relative_path);
  vxcore_string_free(file_id);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_delete_asset"));
  std::cout << "  ✓ test_buffer_delete_asset passed" << std::endl;
  return 0;
}

int test_buffer_get_assets_folder() {
  std::cout << "  Running test_buffer_get_assets_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_assets_folder"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_assets_folder").c_str(),
                               "{\"name\":\"Asset Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a test file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Get assets folder (should create it lazily)
  char *assets_folder = nullptr;
  err = vxcore_buffer_get_assets_folder(ctx, buffer_id, &assets_folder);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(assets_folder);

  // Verify folder exists
  ASSERT_TRUE(path_exists(assets_folder));
  ASSERT_TRUE(std::string(assets_folder).find("vx_assets") != std::string::npos);

  vxcore_string_free(assets_folder);
  vxcore_string_free(file_id);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_assets_folder"));
  std::cout << "  ✓ test_buffer_get_assets_folder passed" << std::endl;
  return 0;
}

// ============ Buffer Attachment Tests (Filesystem + Metadata) ============

int test_buffer_insert_attachment() {
  std::cout << "  Running test_buffer_insert_attachment..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_insert_attachment"));
  create_directory(get_test_path("test_buffer_insert_attachment"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_insert_attachment").c_str(),
                               "{\"name\":\"Attachment Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a test file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create source file
  std::string source_path = get_test_path("test_buffer_insert_attachment") + "/document.pdf";
  write_file(source_path, "PDF content");

  // Insert attachment (copies file + adds to metadata)
  char *filename = nullptr;
  err = vxcore_buffer_insert_attachment(ctx, buffer_id, source_path.c_str(), &filename);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(filename);
  ASSERT_EQ(std::string(filename), "document.pdf");

  const std::string config_path =
      get_test_path("test_buffer_insert_attachment") + "/vx_notebook/contents/vx.json";
  auto config = nlohmann::json::parse(read_file_content(config_path));
  ASSERT_EQ(config.at("files").at(0).at("attachments"), nlohmann::json::array({"document.pdf"}));

  char *attachments_folder = nullptr;
  err = vxcore_buffer_get_attachments_folder(ctx, buffer_id, &attachments_folder);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(attachments_folder);
  ASSERT_EQ(read_file_content(std::string(attachments_folder) + "/" + filename), "PDF content");
  vxcore_string_free(attachments_folder);

  // Verify attachment is in the list
  char *attachments_json = nullptr;
  err = vxcore_buffer_list_attachments(ctx, buffer_id, &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto json = nlohmann::json::parse(attachments_json);
  ASSERT_EQ(json.size(), 1u);
  ASSERT_EQ(json[0].get<std::string>(), "document.pdf");
  vxcore_string_free(attachments_json);

  vxcore_string_free(filename);
  vxcore_string_free(file_id);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_insert_attachment"));
  std::cout << "  ✓ test_buffer_insert_attachment passed" << std::endl;
  return 0;
}

int test_buffer_delete_attachment() {
  std::cout << "  Running test_buffer_delete_attachment..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_delete_attachment"));
  create_directory(get_test_path("test_buffer_delete_attachment"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_delete_attachment").c_str(),
                               "{\"name\":\"Attachment Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a test file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create and insert attachment
  std::string source_path = get_test_path("test_buffer_delete_attachment") + "/to_delete.zip";
  write_file(source_path, "ZIP content");

  char *filename = nullptr;
  err = vxcore_buffer_insert_attachment(ctx, buffer_id, source_path.c_str(), &filename);
  ASSERT_EQ(err, VXCORE_OK);

  // Resolve attachment absolute path in assets folder
  char *attachments_folder = nullptr;
  err = vxcore_buffer_get_attachments_folder(ctx, buffer_id, &attachments_folder);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(attachments_folder);
  std::string attachment_abs_path = std::string(attachments_folder) + "/" + filename;
  ASSERT_TRUE(path_exists(attachment_abs_path));
  vxcore_string_free(attachments_folder);

  // Verify in list
  char *attachments_json = nullptr;
  err = vxcore_buffer_list_attachments(ctx, buffer_id, &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto json = nlohmann::json::parse(attachments_json);
  ASSERT_EQ(json.size(), 1u);
  vxcore_string_free(attachments_json);

  const std::string config_path =
      get_test_path("test_buffer_delete_attachment") + "/vx_notebook/contents/vx.json";
  auto config = nlohmann::json::parse(read_file_content(config_path));
  ASSERT_EQ(config.at("files").at(0).at("attachments"), nlohmann::json::array({"to_delete.zip"}));

  // Delete attachment
  err = vxcore_buffer_delete_attachment(ctx, buffer_id, filename);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify removed from original assets folder
  ASSERT_FALSE(path_exists(attachment_abs_path));

  // Verify moved to recycle bin for bundled notebook
  std::string recycle_bin_path =
      get_test_path("test_buffer_delete_attachment") + "/vx_notebook/recycle_bin/" + filename;
  ASSERT_TRUE(path_exists(recycle_bin_path));

  // Verify removed from list
  err = vxcore_buffer_list_attachments(ctx, buffer_id, &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  json = nlohmann::json::parse(attachments_json);
  ASSERT_EQ(json.size(), 0u);
  vxcore_string_free(attachments_json);

  config = nlohmann::json::parse(read_file_content(config_path));
  ASSERT_FALSE(config.at("files").at(0).contains("attachments"));

  vxcore_string_free(filename);
  vxcore_string_free(file_id);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_delete_attachment"));
  std::cout << "  ✓ test_buffer_delete_attachment passed" << std::endl;
  return 0;
}

int test_buffer_rename_attachment() {
  std::cout << "  Running test_buffer_rename_attachment..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_rename_attachment"));
  create_directory(get_test_path("test_buffer_rename_attachment"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_rename_attachment").c_str(),
                               "{\"name\":\"Attachment Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a test file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create and insert attachment
  std::string source_path = get_test_path("test_buffer_rename_attachment") + "/old_name.pdf";
  write_file(source_path, "PDF content");

  char *filename = nullptr;
  err = vxcore_buffer_insert_attachment(ctx, buffer_id, source_path.c_str(), &filename);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(std::string(filename), "old_name.pdf");

  char *attachments_folder = nullptr;
  err = vxcore_buffer_get_attachments_folder(ctx, buffer_id, &attachments_folder);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(attachments_folder);
  const std::string assets_path = attachments_folder;
  vxcore_string_free(attachments_folder);

  // Rename attachment
  char *new_filename = nullptr;
  err = vxcore_buffer_rename_attachment(ctx, buffer_id, filename, "new_name.pdf", &new_filename);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(new_filename);
  ASSERT_EQ(std::string(new_filename), "new_name.pdf");

  ASSERT_FALSE(path_exists(assets_path + "/old_name.pdf"));
  ASSERT_EQ(read_file_content(assets_path + "/new_name.pdf"), "PDF content");
  const std::string config_path =
      get_test_path("test_buffer_rename_attachment") + "/vx_notebook/contents/vx.json";
  auto config = nlohmann::json::parse(read_file_content(config_path));
  ASSERT_EQ(config.at("files").at(0).at("attachments"), nlohmann::json::array({"new_name.pdf"}));

  // Verify updated in list
  char *attachments_json = nullptr;
  err = vxcore_buffer_list_attachments(ctx, buffer_id, &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto json = nlohmann::json::parse(attachments_json);
  ASSERT_EQ(json.size(), 1u);
  ASSERT_EQ(json[0].get<std::string>(), "new_name.pdf");
  vxcore_string_free(attachments_json);

  vxcore_string_free(new_filename);
  vxcore_string_free(filename);
  vxcore_string_free(file_id);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_rename_attachment"));
  std::cout << "  ✓ test_buffer_rename_attachment passed" << std::endl;
  return 0;
}

int test_buffer_legacy_attachment_metadata() {
  std::cout << "  Running test_buffer_legacy_attachment_metadata..." << std::endl;
  const std::string fixture_path = get_test_path("test_buffer_legacy_attachment_metadata");
  const std::string notebook_path = fixture_path + "/notebook";
  const char *note_path = "parent/nested/note.md";
  const std::string config_path = notebook_path + "/vx_notebook/contents/parent/nested/vx.json";
  cleanup_test_dir(fixture_path);

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  char *notebook_id = nullptr;
  err = vxcore_notebook_create(
      ctx, notebook_path.c_str(),
      R"({"name":"Legacy Attachments","assetsFolder":"../../../shared-assets"})",
      VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  char *folder_id = nullptr;
  err = vxcore_folder_create_path(ctx, notebook_id, "parent/nested", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(folder_id);
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "parent/nested", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(file_id);
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, note_path, &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  const std::string source_path = notebook_path + "/document.pdf";
  write_file(source_path, "attachment content");
  char *filename = nullptr;
  err = vxcore_buffer_insert_attachment(ctx, buffer_id, source_path.c_str(), &filename);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(filename);
  ASSERT_EQ(std::string(filename), "document.pdf");
  vxcore_string_free(filename);

  // An untracked generic asset also supplies the later rename collision.
  const std::string generic_source = notebook_path + "/occupied.pdf";
  write_file(generic_source, "unrelated generic asset");
  char *generic_relative_path = nullptr;
  err = vxcore_buffer_insert_asset(ctx, buffer_id, generic_source.c_str(), &generic_relative_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(generic_relative_path);
  const std::string generic_path = notebook_path + "/" + generic_relative_path;
  vxcore_string_free(generic_relative_path);
  ASSERT_EQ(read_file_content(generic_path), "unrelated generic asset");

  char *attachments_folder = nullptr;
  err = vxcore_buffer_get_attachments_folder(ctx, buffer_id, &attachments_folder);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(attachments_folder);
  const std::string assets_path = attachments_folder;
  vxcore_string_free(attachments_folder);
  ASSERT_EQ(read_file_content(assets_path + "/document.pdf"), "attachment content");
  ASSERT_EQ(read_file_content(assets_path + "/occupied.pdf"), "unrelated generic asset");

  // Tear down all cached folder state before replacing the existing disk fixture.
  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(buffer_id);
  buffer_id = nullptr;
  vxcore_string_free(notebook_id);
  notebook_id = nullptr;
  vxcore_context_destroy(ctx);
  ctx = nullptr;

  ASSERT_TRUE(path_exists(config_path));
  auto config = nlohmann::json::parse(read_file_content(config_path));
  ASSERT_EQ(config.at("files").at(0).at("name"), "note.md");
  const std::string windows_prefix =
      "parent\\nested\\old-assets\\00000000-0000-4000-8000-000000000001\\";
  const std::string posix_prefix =
      "other/parent/custom-assets/00000000-0000-4000-8000-000000000002/";
  config.at("files").at(0)["attachments"] = nlohmann::json::array(
      {"../shared-assets/" + config.at("files").at(0).at("id").get<std::string>() + "/document.pdf",
       windows_prefix + "document.pdf", "document.pdf", posix_prefix + "document.pdf"});
  config["modifiedUtc"] = config.at("modifiedUtc").get<int64_t>() + 1000;
  const std::string legacy_bytes = config.dump(2);
  write_file(config_path, legacy_bytes);

  err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_notebook_open(ctx, notebook_path.c_str(), &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_open(ctx, notebook_id, note_path, &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  char *attachments_json = nullptr;
  err = vxcore_buffer_list_attachments(ctx, buffer_id, &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(attachments_json), nlohmann::json::array({"document.pdf"}));
  vxcore_string_free(attachments_json);
  err = vxcore_node_list_attachments(ctx, notebook_id, note_path, &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(attachments_json), nlohmann::json::array({"document.pdf"}));
  vxcore_string_free(attachments_json);
  ASSERT_EQ(read_file_content(config_path), legacy_bytes);

  // Adding the canonical name of a legacy entry is a true no-op, not a migration save.
  err = vxcore_file_add_attachment(ctx, notebook_id, note_path, "document.pdf");
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(read_file_content(config_path), legacy_bytes);
  err = vxcore_buffer_list_attachments(ctx, buffer_id, &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(attachments_json), nlohmann::json::array({"document.pdf"}));
  vxcore_string_free(attachments_json);

  // Reject invalid basenames before touching the physical attachment or its metadata.
  char *new_filename = nullptr;
  err = vxcore_buffer_rename_attachment(ctx, buffer_id, "document.pdf", "C:escape.pdf",
                                        &new_filename);
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  err = vxcore_buffer_delete_attachment(ctx, buffer_id, "../document.pdf");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(read_file_content(assets_path + "/document.pdf"), "attachment content");
  ASSERT_EQ(read_file_content(config_path), legacy_bytes);

  err = vxcore_buffer_rename_attachment(ctx, buffer_id, "document.pdf", "occupied.pdf",
                                        &new_filename);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(new_filename);
  const std::string renamed_name = new_filename;
  vxcore_string_free(new_filename);
  ASSERT_NE(renamed_name, "occupied.pdf");
  ASSERT_NE(renamed_name, "document.pdf");
  ASSERT_EQ(renamed_name.find_first_of("/\\"), std::string::npos);
  ASSERT_FALSE(path_exists(assets_path + "/document.pdf"));
  ASSERT_EQ(read_file_content(assets_path + "/" + renamed_name), "attachment content");
  ASSERT_EQ(read_file_content(generic_path), "unrelated generic asset");
  config = nlohmann::json::parse(read_file_content(config_path));
  ASSERT_EQ(config.at("files").at(0).at("attachments"), nlohmann::json::array({renamed_name}));
  err = vxcore_buffer_list_attachments(ctx, buffer_id, &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(attachments_json), nlohmann::json::array({renamed_name}));
  vxcore_string_free(attachments_json);

  err = vxcore_buffer_delete_attachment(ctx, buffer_id, renamed_name.c_str());
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_FALSE(path_exists(assets_path + "/" + renamed_name));
  ASSERT_EQ(read_file_content(notebook_path + "/vx_notebook/recycle_bin/" + renamed_name),
            "attachment content");
  ASSERT_EQ(read_file_content(generic_path), "unrelated generic asset");
  config = nlohmann::json::parse(read_file_content(config_path));
  ASSERT_FALSE(config.at("files").at(0).contains("attachments"));
  err = vxcore_buffer_list_attachments(ctx, buffer_id, &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(attachments_json), nlohmann::json::array());
  vxcore_string_free(attachments_json);

  // Metadata-only replacement normalizes both separator styles and preserves first-seen order.
  const auto replacement = nlohmann::json::array(
      {windows_prefix + "report.zip", "document.pdf", posix_prefix + "document.pdf", "report.zip"});
  err = vxcore_file_update_attachments(ctx, notebook_id, note_path, replacement.dump().c_str());
  ASSERT_EQ(err, VXCORE_OK);
  auto expected = nlohmann::json::array({"report.zip", "document.pdf"});
  config = nlohmann::json::parse(read_file_content(config_path));
  ASSERT_EQ(config.at("files").at(0).at("attachments"), expected);
  err = vxcore_node_list_attachments(ctx, notebook_id, note_path, &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(attachments_json), expected);
  vxcore_string_free(attachments_json);

  err =
      vxcore_file_add_attachment(ctx, notebook_id, note_path, (posix_prefix + "third.txt").c_str());
  ASSERT_EQ(err, VXCORE_OK);
  expected.push_back("third.txt");
  config = nlohmann::json::parse(read_file_content(config_path));
  ASSERT_EQ(config.at("files").at(0).at("attachments"), expected);
  const std::string added_bytes = read_file_content(config_path);
  err = vxcore_file_add_attachment(ctx, notebook_id, note_path,
                                   (windows_prefix + "third.txt").c_str());
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(read_file_content(config_path), added_bytes);

  err = vxcore_file_delete_attachment(ctx, notebook_id, note_path,
                                      (windows_prefix + "document.pdf").c_str());
  ASSERT_EQ(err, VXCORE_OK);
  expected = nlohmann::json::array({"report.zip", "third.txt"});
  config = nlohmann::json::parse(read_file_content(config_path));
  ASSERT_EQ(config.at("files").at(0).at("attachments"), expected);

  // Reject traversal before changing either the live list or the persisted record.
  const std::string valid_bytes = read_file_content(config_path);
  err = vxcore_file_add_attachment(ctx, notebook_id, note_path, "..\\outside.pdf");
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(read_file_content(config_path), valid_bytes);
  err = vxcore_file_delete_attachment(ctx, notebook_id, note_path,
                                      (posix_prefix + "../report.zip").c_str());
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(read_file_content(config_path), valid_bytes);
  const auto invalid_replacement =
      nlohmann::json::array({"replacement.zip", windows_prefix + "..\\third.txt"});
  err = vxcore_file_update_attachments(ctx, notebook_id, note_path,
                                       invalid_replacement.dump().c_str());
  ASSERT_EQ(err, VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(read_file_content(config_path), valid_bytes);
  err = vxcore_node_list_attachments(ctx, notebook_id, note_path, &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(attachments_json), expected);
  vxcore_string_free(attachments_json);
  ASSERT_EQ(read_file_content(generic_path), "unrelated generic asset");

  err = vxcore_file_update_attachments(ctx, notebook_id, note_path, "[]");
  ASSERT_EQ(err, VXCORE_OK);
  config = nlohmann::json::parse(read_file_content(config_path));
  ASSERT_FALSE(config.at("files").at(0).contains("attachments"));
  ASSERT_EQ(read_file_content(generic_path), "unrelated generic asset");

  err = vxcore_notebook_close(ctx, notebook_id);
  ASSERT_EQ(err, VXCORE_OK);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(fixture_path);
  std::cout << "  test_buffer_legacy_attachment_metadata passed" << std::endl;
  return 0;
}

int test_buffer_list_attachments() {
  std::cout << "  Running test_buffer_list_attachments..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_list_attachments"));
  create_directory(get_test_path("test_buffer_list_attachments"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_list_attachments").c_str(),
                               "{\"name\":\"Attachment Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a test file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create and insert multiple attachments
  std::string source1 = get_test_path("test_buffer_list_attachments") + "/doc1.pdf";
  std::string source2 = get_test_path("test_buffer_list_attachments") + "/doc2.zip";
  write_file(source1, "PDF content");
  write_file(source2, "ZIP content");

  char *filename1 = nullptr;
  char *filename2 = nullptr;
  err = vxcore_buffer_insert_attachment(ctx, buffer_id, source1.c_str(), &filename1);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_insert_attachment(ctx, buffer_id, source2.c_str(), &filename2);
  ASSERT_EQ(err, VXCORE_OK);

  // List attachments
  char *attachments_json = nullptr;
  err = vxcore_buffer_list_attachments(ctx, buffer_id, &attachments_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(attachments_json);

  auto json = nlohmann::json::parse(attachments_json);
  ASSERT_TRUE(json.is_array());
  ASSERT_EQ(json.size(), 2u);

  vxcore_string_free(attachments_json);
  vxcore_string_free(filename1);
  vxcore_string_free(filename2);
  vxcore_string_free(file_id);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_list_attachments"));
  std::cout << "  ✓ test_buffer_list_attachments passed" << std::endl;
  return 0;
}

int test_buffer_list_unindexed_attachments() {
  std::cout << "  Running test_buffer_list_unindexed_attachments..." << std::endl;
  const std::string notebook_path = get_test_path("test_buffer_list_unindexed_attachments");
  const std::string config_path = notebook_path + "/vx_notebook/contents/vx.json";
  const std::string note_path = notebook_path + "/note.md";
  const std::string note_content = "# Attachment scan\n![unused](unused.png)\n";
  cleanup_test_dir(notebook_path);

  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Attachment Scan\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id), VXCORE_OK);
  write_file(note_path, note_content);
  ASSERT_EQ(read_file_content(note_path), note_content);
  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, notebook_id, "note.md", &buffer_id), VXCORE_OK);

  char *folder = nullptr;
  ASSERT_EQ(vxcore_node_get_attachments_folder(ctx, notebook_id, "note.md", &folder), VXCORE_OK);
  ASSERT_NOT_NULL(folder);
  const std::string assets_path = folder;
  vxcore_string_free(folder);
  ASSERT_FALSE(path_exists(assets_path));
  const auto empty_tree = snapshot_attachment_test_tree(notebook_path);
  char *attachments_json = nullptr;
  ASSERT_EQ(vxcore_buffer_list_unindexed_attachments(ctx, buffer_id, &attachments_json), VXCORE_OK);
  ASSERT_NOT_NULL(attachments_json);
  ASSERT_EQ(nlohmann::json::parse(attachments_json), nlohmann::json::array());
  vxcore_string_free(attachments_json);
  ASSERT_FALSE(path_exists(assets_path));
  ASSERT_EQ(snapshot_attachment_test_tree(notebook_path), empty_tree);

  create_directory(assets_path + "/nested");
  const std::string unicode_name = "资料 résumé 01.pdf";
  const auto contents = nlohmann::json::object({{"indexed.pdf", "indexed PDF bytes"},
                                                {"loose.pdf", "loose PDF bytes"},
                                                {"unused.png", std::string("PNG\0bytes", 9)},
                                                {unicode_name, "Unicode filename bytes"},
                                                {"nested/child.txt", "nested bytes"}});
  for (auto it = contents.begin(); it != contents.end(); ++it) {
    write_file(assets_path + "/" + it.key(), it.value().get<std::string>());
    ASSERT_EQ(read_file_content(assets_path + "/" + it.key()), it.value().get<std::string>());
  }
  ASSERT_EQ(vxcore_file_add_attachment(ctx, notebook_id, "note.md", "indexed.pdf"), VXCORE_OK);

  // A file symlink must not become an attachment, even if its target is eligible.
  std::error_code link_error;
  std::filesystem::create_symlink(utf8_to_fs_path(assets_path + "/loose.pdf"),
                                  utf8_to_fs_path(assets_path + "/linked.pdf"), link_error);
  if (link_error) {
    bool unavailable = link_error == std::errc::permission_denied ||
                       link_error == std::errc::operation_not_permitted ||
                       link_error == std::errc::operation_not_supported;
#ifdef _WIN32
    unavailable = unavailable || link_error.value() == ERROR_PRIVILEGE_NOT_HELD;
#endif
    ASSERT_TRUE(unavailable);
    std::cout << "    SKIP file symlink exclusion: platform/privilege unavailable: "
              << link_error.message() << std::endl;
  } else {
    ASSERT_TRUE(std::filesystem::is_symlink(utf8_to_fs_path(assets_path + "/linked.pdf")));
  }

  const auto original_tree = snapshot_attachment_test_tree(notebook_path);
  const auto original_assets = snapshot_attachment_test_tree(assets_path);
  ASSERT_EQ(vxcore_buffer_list_unindexed_attachments(ctx, buffer_id, &attachments_json), VXCORE_OK);
  ASSERT_NOT_NULL(attachments_json);
  ASSERT_EQ(nlohmann::json::parse(attachments_json),
            nlohmann::json::array({"loose.pdf", "unused.png", unicode_name}));
  vxcore_string_free(attachments_json);
  ASSERT_EQ(vxcore_buffer_list_attachments(ctx, buffer_id, &attachments_json), VXCORE_OK);
  ASSERT_NOT_NULL(attachments_json);
  ASSERT_EQ(nlohmann::json::parse(attachments_json), nlohmann::json::array({"indexed.pdf"}));
  vxcore_string_free(attachments_json);
  ASSERT_EQ(snapshot_attachment_test_tree(notebook_path), original_tree);

  // Register in place twice: exactly one metadata entry, no copy or suffixed filename.
  ASSERT_EQ(vxcore_file_add_attachment(ctx, notebook_id, "note.md", "loose.pdf"), VXCORE_OK);
  const auto registered_tree = snapshot_attachment_test_tree(notebook_path);
  ASSERT_EQ(vxcore_file_add_attachment(ctx, notebook_id, "note.md", "loose.pdf"), VXCORE_OK);
  ASSERT_EQ(snapshot_attachment_test_tree(notebook_path), registered_tree);
  ASSERT_EQ(vxcore_buffer_list_unindexed_attachments(ctx, buffer_id, &attachments_json), VXCORE_OK);
  ASSERT_NOT_NULL(attachments_json);
  ASSERT_EQ(nlohmann::json::parse(attachments_json),
            nlohmann::json::array({"unused.png", unicode_name}));
  vxcore_string_free(attachments_json);
  ASSERT_EQ(vxcore_buffer_list_attachments(ctx, buffer_id, &attachments_json), VXCORE_OK);
  ASSERT_NOT_NULL(attachments_json);
  const auto expected_index = nlohmann::json::array({"indexed.pdf", "loose.pdf"});
  ASSERT_EQ(nlohmann::json::parse(attachments_json), expected_index);
  vxcore_string_free(attachments_json);
  ASSERT_EQ(
      nlohmann::json::parse(read_file_content(config_path)).at("files").at(0).at("attachments"),
      expected_index);
  ASSERT_EQ(snapshot_attachment_test_tree(assets_path), original_assets);
  ASSERT_EQ(read_file_content(note_path), note_content);
  ASSERT_EQ(snapshot_attachment_test_tree(notebook_path), registered_tree);

  // The core reports filesystem facts: hidden files and VNote-owned sidecars are not filtered.
  write_file(assets_path + "/.hidden.txt", "ordinary hidden file");
  write_file(assets_path + "/comments.json", "{}");
  write_file(assets_path + "/.gitkeep", "");
  const auto sidecar_tree = snapshot_attachment_test_tree(notebook_path);
  ASSERT_EQ(vxcore_buffer_list_unindexed_attachments(ctx, buffer_id, &attachments_json), VXCORE_OK);
  ASSERT_NOT_NULL(attachments_json);
  ASSERT_EQ(nlohmann::json::parse(attachments_json),
            nlohmann::json::array(
                {".gitkeep", ".hidden.txt", "comments.json", "unused.png", unicode_name}));
  vxcore_string_free(attachments_json);
  ASSERT_EQ(snapshot_attachment_test_tree(notebook_path), sidecar_tree);

  ASSERT_EQ(vxcore_buffer_close(ctx, buffer_id), VXCORE_OK);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(notebook_path);
  std::cout << "  test_buffer_list_unindexed_attachments passed" << std::endl;
  return 0;
}

int test_buffer_list_unindexed_attachments_errors() {
  std::cout << "  Running test_buffer_list_unindexed_attachments_errors..." << std::endl;
  const std::string notebook_path = get_test_path("test_buffer_list_unindexed_attachments_errors");
  cleanup_test_dir(notebook_path);
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *notebook_id = nullptr;
  ASSERT_EQ(vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Scan Errors\"}",
                                   VXCORE_NOTEBOOK_BUNDLED, &notebook_id),
            VXCORE_OK);
  char *file_id = nullptr;
  ASSERT_EQ(vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id), VXCORE_OK);
  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, notebook_id, "note.md", &buffer_id), VXCORE_OK);
  const auto original_tree = snapshot_attachment_test_tree(notebook_path);

  // Start with non-null outputs so failures must actively clear them.
  char sentinel = '\0';
  char *attachments_json = &sentinel;
  ASSERT_EQ(vxcore_buffer_list_unindexed_attachments(nullptr, buffer_id, &attachments_json),
            VXCORE_ERR_NULL_POINTER);
  ASSERT_NULL(attachments_json);
  attachments_json = &sentinel;
  ASSERT_EQ(vxcore_buffer_list_unindexed_attachments(ctx, nullptr, &attachments_json),
            VXCORE_ERR_NULL_POINTER);
  ASSERT_NULL(attachments_json);
  ASSERT_EQ(vxcore_buffer_list_unindexed_attachments(ctx, buffer_id, nullptr),
            VXCORE_ERR_NULL_POINTER);
  attachments_json = &sentinel;
  ASSERT_EQ(vxcore_buffer_list_unindexed_attachments(ctx, "missing-buffer", &attachments_json),
            VXCORE_ERR_UNSUPPORTED);
  ASSERT_NULL(attachments_json);
  ASSERT_EQ(snapshot_attachment_test_tree(notebook_path), original_tree);

  char *folder = nullptr;
  ASSERT_EQ(vxcore_node_get_attachments_folder(ctx, notebook_id, "note.md", &folder), VXCORE_OK);
  ASSERT_NOT_NULL(folder);
  const std::string assets_path = folder;
  vxcore_string_free(folder);
  ASSERT_FALSE(path_exists(assets_path));
  std::filesystem::create_directories(utf8_to_fs_path(assets_path).parent_path());
  write_file(assets_path, "not a directory");
  ASSERT_EQ(read_file_content(assets_path), "not a directory");
  const auto blocked_tree = snapshot_attachment_test_tree(notebook_path);
  attachments_json = &sentinel;
  ASSERT_EQ(vxcore_buffer_list_unindexed_attachments(ctx, buffer_id, &attachments_json),
            VXCORE_ERR_IO);
  ASSERT_NULL(attachments_json);
  ASSERT_EQ(snapshot_attachment_test_tree(notebook_path), blocked_tree);

  ASSERT_EQ(vxcore_buffer_close(ctx, buffer_id), VXCORE_OK);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(notebook_path);
  std::cout << "  test_buffer_list_unindexed_attachments_errors passed" << std::endl;
  return 0;
}

int test_buffer_list_unindexed_attachments_external_unsupported() {
  std::cout << "  Running test_buffer_list_unindexed_attachments_external_unsupported..."
            << std::endl;
  const std::string fixture_path = get_test_path("test_buffer_unindexed_external");
  const std::string note_path = fixture_path + "/external.md";
  cleanup_test_dir(fixture_path);
  create_directory(fixture_path);
  write_file(note_path, "External note bytes");
  VxCoreContextHandle ctx = nullptr;
  ASSERT_EQ(vxcore_context_create(nullptr, &ctx), VXCORE_OK);
  char *buffer_id = nullptr;
  ASSERT_EQ(vxcore_buffer_open(ctx, nullptr, note_path.c_str(), &buffer_id), VXCORE_OK);

  const auto original_tree = snapshot_attachment_test_tree(fixture_path);
  char sentinel = '\0';
  char *attachments_json = &sentinel;
  ASSERT_EQ(vxcore_buffer_list_unindexed_attachments(ctx, buffer_id, &attachments_json),
            VXCORE_ERR_UNSUPPORTED);
  ASSERT_NULL(attachments_json);
  ASSERT_EQ(snapshot_attachment_test_tree(fixture_path), original_tree);

  // Even when an external attachment directory exists, there is no authoritative index.
  char *folder = nullptr;
  ASSERT_EQ(vxcore_buffer_get_attachments_folder(ctx, buffer_id, &folder), VXCORE_OK);
  ASSERT_NOT_NULL(folder);
  write_file(std::string(folder) + "/loose.txt", "external attachment bytes");
  ASSERT_EQ(read_file_content(std::string(folder) + "/loose.txt"), "external attachment bytes");
  vxcore_string_free(folder);
  const auto populated_tree = snapshot_attachment_test_tree(fixture_path);
  attachments_json = &sentinel;
  ASSERT_EQ(vxcore_buffer_list_unindexed_attachments(ctx, buffer_id, &attachments_json),
            VXCORE_ERR_UNSUPPORTED);
  ASSERT_NULL(attachments_json);
  ASSERT_EQ(snapshot_attachment_test_tree(fixture_path), populated_tree);

  ASSERT_EQ(vxcore_buffer_close(ctx, buffer_id), VXCORE_OK);
  vxcore_string_free(buffer_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(fixture_path);
  std::cout << "  test_buffer_list_unindexed_attachments_external_unsupported passed" << std::endl;
  return 0;
}

int test_buffer_get_attachments_folder() {
  std::cout << "  Running test_buffer_get_attachments_folder..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_attachments_folder"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_attachments_folder").c_str(),
                               "{\"name\":\"Attachment Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a test file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Get attachments folder (should create it lazily)
  char *attachments_folder = nullptr;
  err = vxcore_buffer_get_attachments_folder(ctx, buffer_id, &attachments_folder);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(attachments_folder);

  // Verify folder exists and is same as assets folder
  ASSERT_TRUE(path_exists(attachments_folder));
  ASSERT_TRUE(std::string(attachments_folder).find("vx_assets") != std::string::npos);

  vxcore_string_free(attachments_folder);
  vxcore_string_free(file_id);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_attachments_folder"));
  std::cout << "  ✓ test_buffer_get_attachments_folder passed" << std::endl;
  return 0;
}

int test_buffer_external_asset() {
  std::cout << "  Running test_buffer_external_asset..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_external_asset"));
  create_directory(get_test_path("test_buffer_external_asset"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Create an external file
  std::string ext_file_path = get_test_path("test_buffer_external_asset") + "/notes.md";
  write_file(ext_file_path, "# External Notes\n");

  // Open buffer for external file (notebook_id = NULL)
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, nullptr, ext_file_path.c_str(), &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Insert asset (raw binary data)
  const uint8_t data[] = {0x89, 'P', 'N', 'G'};
  char *relative_path = nullptr;
  err = vxcore_buffer_insert_asset_raw(ctx, buffer_id, "image.png", data, sizeof(data),
                                       &relative_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(relative_path);

  // Verify relative path format (should be notes_assets/image.png)
  std::string rel_path(relative_path);
  ASSERT_TRUE(rel_path.find("notes_assets") != std::string::npos);
  ASSERT_TRUE(rel_path.find("image.png") != std::string::npos);

  // Verify file was created
  std::string asset_abs_path = get_test_path("test_buffer_external_asset") + "/" + rel_path;
  ASSERT_TRUE(path_exists(asset_abs_path));

  vxcore_string_free(relative_path);
  vxcore_string_free(buffer_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_external_asset"));
  std::cout << "  ✓ test_buffer_external_asset passed" << std::endl;
  return 0;
}

int test_buffer_asset_unique_name() {
  std::cout << "  Running test_buffer_asset_unique_name..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_asset_unique"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_asset_unique").c_str(),
                               "{\"name\":\"Asset Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a test file
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Insert same-named assets multiple times (using raw)
  const uint8_t data[] = {0x89, 'P', 'N', 'G'};
  char *path1 = nullptr;
  char *path2 = nullptr;
  char *path3 = nullptr;

  err = vxcore_buffer_insert_asset_raw(ctx, buffer_id, "image.png", data, sizeof(data), &path1);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_insert_asset_raw(ctx, buffer_id, "image.png", data, sizeof(data), &path2);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_insert_asset_raw(ctx, buffer_id, "image.png", data, sizeof(data), &path3);
  ASSERT_EQ(err, VXCORE_OK);

  // All paths should be different
  ASSERT_NE(std::string(path1), std::string(path2));
  ASSERT_NE(std::string(path2), std::string(path3));
  ASSERT_NE(std::string(path1), std::string(path3));

  // Should have image.png, image_1.png, image_2.png
  ASSERT_TRUE(std::string(path1).find("image.png") != std::string::npos);
  ASSERT_TRUE(std::string(path2).find("image_1.png") != std::string::npos);
  ASSERT_TRUE(std::string(path3).find("image_2.png") != std::string::npos);

  vxcore_string_free(path1);
  vxcore_string_free(path2);
  vxcore_string_free(path3);
  vxcore_string_free(file_id);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_asset_unique"));
  std::cout << "  ✓ test_buffer_asset_unique_name passed" << std::endl;
  return 0;
}

int test_buffer_backup_cleanup_on_save() {
  std::cout << "  Running test_buffer_backup_cleanup_on_save..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_backup_cleanup_on_save"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_backup_cleanup_on_save");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Backup Cleanup Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  const char *content = "cleanup save test";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, content, strlen(content));
  ASSERT_EQ(err, VXCORE_OK);

  // Write backup and verify it exists
  err = vxcore_buffer_write_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string backup_path = notebook_path + "/test.md.vswp";
  ASSERT_TRUE(std::filesystem::exists(backup_path));

  // Save buffer — should auto-delete backup
  err = vxcore_buffer_save(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify backup was auto-deleted
  ASSERT_FALSE(std::filesystem::exists(backup_path));

  // Verify original file has the saved content
  std::string file_path = notebook_path + "/test.md";
  std::string disk_content = read_file_content(file_path);
  ASSERT_EQ(disk_content, std::string(content));

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_backup_cleanup_on_save"));
  std::cout << "  ✓ test_buffer_backup_cleanup_on_save passed" << std::endl;
  return 0;
}

int test_buffer_backup_cleanup_on_close() {
  std::cout << "  Running test_buffer_backup_cleanup_on_close..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_backup_cleanup_on_close"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_backup_cleanup_on_close");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Backup Cleanup Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  const char *content = "cleanup close test";
  err = vxcore_buffer_set_content_raw(ctx, buffer_id, content, strlen(content));
  ASSERT_EQ(err, VXCORE_OK);

  // Write backup and verify it exists
  err = vxcore_buffer_write_backup(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string backup_path = notebook_path + "/test.md.vswp";
  ASSERT_TRUE(std::filesystem::exists(backup_path));

  // Close buffer — should auto-delete backup
  err = vxcore_buffer_close(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify backup was auto-deleted
  ASSERT_FALSE(std::filesystem::exists(backup_path));

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_backup_cleanup_on_close"));
  std::cout << "  ✓ test_buffer_backup_cleanup_on_close passed" << std::endl;
  return 0;
}

int test_buffer_rename_file_updates_path() {
  std::cout << "  Running test_buffer_rename_file_updates_path..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_rename_file_updates_path"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_rename_file_updates_path").c_str(),
                               "{\"name\":\"Rename Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file and open its buffer
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id);

  // Verify initial filePath is "test.md"
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json buf_data = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data["filePath"].get<std::string>(), std::string("test.md"));
  vxcore_string_free(buffer_json);

  // Save the buffer ID for later comparison
  std::string original_buffer_id(buffer_id);

  // Rename the file
  err = vxcore_node_rename(ctx, notebook_id, "test.md", "renamed.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify buffer's filePath is now "renamed.md"
  buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  buf_data = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data["filePath"].get<std::string>(), std::string("renamed.md"));
  vxcore_string_free(buffer_json);

  // Verify buffer ID is unchanged
  ASSERT_EQ(buf_data["id"].get<std::string>(), original_buffer_id);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_rename_file_updates_path"));
  std::cout << "  ✓ test_buffer_rename_file_updates_path passed" << std::endl;
  return 0;
}

int test_buffer_rename_folder_updates_paths() {
  std::cout << "  Running test_buffer_rename_folder_updates_paths..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_rename_folder_updates_paths"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(
      ctx, get_test_path("test_buffer_rename_folder_updates_paths").c_str(),
      "{\"name\":\"Rename Folder Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create folder "docs" and two files inside it
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id1 = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "docs", "note.md", &file_id1);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id2 = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "docs", "readme.md", &file_id2);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffers for both files
  char *buffer_id1 = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "docs/note.md", &buffer_id1);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id2 = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "docs/readme.md", &buffer_id2);
  ASSERT_EQ(err, VXCORE_OK);

  // Save buffer IDs for later comparison
  std::string original_buffer_id1(buffer_id1);
  std::string original_buffer_id2(buffer_id2);

  // Rename folder "docs" to "documentation"
  err = vxcore_node_rename(ctx, notebook_id, "docs", "documentation");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify first buffer's filePath is "documentation/note.md"
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id1, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json buf_data1 = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data1["filePath"].get<std::string>(), std::string("documentation/note.md"));
  vxcore_string_free(buffer_json);

  // Verify second buffer's filePath is "documentation/readme.md"
  buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id2, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json buf_data2 = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data2["filePath"].get<std::string>(), std::string("documentation/readme.md"));
  vxcore_string_free(buffer_json);

  // Verify buffer IDs are unchanged
  ASSERT_EQ(buf_data1["id"].get<std::string>(), original_buffer_id1);
  ASSERT_EQ(buf_data2["id"].get<std::string>(), original_buffer_id2);

  // Verify provider is functional after rename: insert asset into buffer1
  // This proves the StandardBufferProvider's cached file_path_ was updated
  const uint8_t asset_data[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  char *relative_path = nullptr;
  err = vxcore_buffer_insert_asset_raw(ctx, buffer_id1, "post_rename.png", asset_data,
                                       sizeof(asset_data), &relative_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(relative_path);

  // Verify asset was created under the renamed folder path
  std::string rel_path(relative_path);
  ASSERT_TRUE(rel_path.find("vx_assets") != std::string::npos);
  ASSERT_TRUE(rel_path.find("post_rename.png") != std::string::npos);

  // Verify the file actually exists on disk
  std::string notebook_root = get_test_path("test_buffer_rename_folder_updates_paths");
  std::string asset_abs_path = notebook_root + "/" + rel_path;
  ASSERT_TRUE(path_exists(asset_abs_path));

  vxcore_string_free(relative_path);
  vxcore_string_free(buffer_id1);
  vxcore_string_free(buffer_id2);
  vxcore_string_free(file_id1);
  vxcore_string_free(file_id2);
  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_rename_folder_updates_paths"));
  std::cout << "  ✓ test_buffer_rename_folder_updates_paths passed" << std::endl;
  return 0;
}

int test_buffer_rename_no_affect_other_buffers() {
  std::cout << "  Running test_buffer_rename_no_affect_other_buffers..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_rename_no_affect_other"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_rename_no_affect_other").c_str(),
                               "{\"name\":\"Rename Isolation Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create two files
  char *file_id_a = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "a.md", &file_id_a);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id_b = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "b.md", &file_id_b);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffers for both
  char *buffer_id_a = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "a.md", &buffer_id_a);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id_b = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "b.md", &buffer_id_b);
  ASSERT_EQ(err, VXCORE_OK);

  // Rename "a.md" to "c.md"
  err = vxcore_node_rename(ctx, notebook_id, "a.md", "c.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify buffer A now has filePath "c.md"
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id_a, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json buf_data_a = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data_a["filePath"].get<std::string>(), std::string("c.md"));
  vxcore_string_free(buffer_json);

  // Verify buffer B still has filePath "b.md" (unaffected)
  buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id_b, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json buf_data_b = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data_b["filePath"].get<std::string>(), std::string("b.md"));
  vxcore_string_free(buffer_json);

  vxcore_string_free(buffer_id_a);
  vxcore_string_free(buffer_id_b);
  vxcore_string_free(file_id_a);
  vxcore_string_free(file_id_b);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_rename_no_affect_other"));
  std::cout << "  ✓ test_buffer_rename_no_affect_other_buffers passed" << std::endl;
  return 0;
}

int test_buffer_rename_provider_functional() {
  std::cout << "  Running test_buffer_rename_provider_functional..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_rename_provider"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_rename_provider").c_str(),
                               "{\"name\":\"Provider Rename Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create folder "notes" and a file inside it
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "notes", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "notes", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer for "notes/test.md"
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "notes/test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id);

  // Verify initial filePath
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json buf_data = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data["filePath"].get<std::string>(), std::string("notes/test.md"));
  vxcore_string_free(buffer_json);

  // Step 3: Insert asset BEFORE rename — verify it works and path relates to "notes/test.md"
  const uint8_t image_data1[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  char *pre_rename_asset_path = nullptr;
  err = vxcore_buffer_insert_asset_raw(ctx, buffer_id, "before_rename.png", image_data1,
                                       sizeof(image_data1), &pre_rename_asset_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(pre_rename_asset_path);

  // Verify pre-rename asset path format
  std::string pre_rel_path(pre_rename_asset_path);
  ASSERT_TRUE(pre_rel_path.find("vx_assets") != std::string::npos);
  ASSERT_TRUE(pre_rel_path.find("before_rename.png") != std::string::npos);

  // Verify pre-rename asset exists on disk
  std::string notebook_root = get_test_path("test_buffer_rename_provider");
  std::string pre_asset_abs = notebook_root + "/" + pre_rel_path;
  ASSERT_TRUE(path_exists(pre_asset_abs));

  // Get assets folder BEFORE rename for comparison
  char *pre_assets_folder = nullptr;
  err = vxcore_buffer_get_assets_folder(ctx, buffer_id, &pre_assets_folder);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(pre_assets_folder);
  std::string pre_assets_folder_str(pre_assets_folder);
  vxcore_string_free(pre_assets_folder);

  // Step 4: Rename the file from "notes/test.md" to "renamed.md"
  err = vxcore_node_rename(ctx, notebook_id, "notes/test.md", "renamed.md");
  ASSERT_EQ(err, VXCORE_OK);

  // Step 5: Verify buffer filePath updated
  buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  buf_data = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data["filePath"].get<std::string>(), std::string("notes/renamed.md"));
  vxcore_string_free(buffer_json);

  // Step 6: Insert asset AFTER rename — proves provider uses updated path
  const uint8_t image_data2[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46};
  char *post_rename_asset_path = nullptr;
  err = vxcore_buffer_insert_asset_raw(ctx, buffer_id, "after_rename.jpg", image_data2,
                                       sizeof(image_data2), &post_rename_asset_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(post_rename_asset_path);

  // Verify post-rename asset path format
  std::string post_rel_path(post_rename_asset_path);
  ASSERT_TRUE(post_rel_path.find("vx_assets") != std::string::npos);
  ASSERT_TRUE(post_rel_path.find("after_rename.jpg") != std::string::npos);

  // Verify post-rename asset exists on disk
  std::string post_asset_abs = notebook_root + "/" + post_rel_path;
  ASSERT_TRUE(path_exists(post_asset_abs));

  // Step 7: Get assets folder AFTER rename — verify it resolves to new location
  char *post_assets_folder = nullptr;
  err = vxcore_buffer_get_assets_folder(ctx, buffer_id, &post_assets_folder);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(post_assets_folder);
  std::string post_assets_folder_str(post_assets_folder);

  // The assets folder should reference the renamed file's location
  ASSERT_TRUE(path_exists(post_assets_folder_str));
  ASSERT_TRUE(post_assets_folder_str.find("vx_assets") != std::string::npos);

  // Assets folder uses UUID-based path, so it should NOT change on file rename
  ASSERT_EQ(pre_assets_folder_str, post_assets_folder_str);

  vxcore_string_free(post_assets_folder);

  // Step 8: Cleanup
  vxcore_string_free(pre_rename_asset_path);
  vxcore_string_free(post_rename_asset_path);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_rename_provider"));
  std::cout << "  ✓ test_buffer_rename_provider_functional passed" << std::endl;
  return 0;
}

// ============ Buffer Path Update on Move Tests ============

int test_buffer_move_file_updates_path() {
  std::cout << "  Running test_buffer_move_file_updates_path..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_move_file_updates_path"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_move_file_updates_path").c_str(),
                               "{\"name\":\"Move File Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create subfolder "sub" and file "sub/test.md"
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "sub", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "sub", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer for "sub/test.md"
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "sub/test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id);

  // Verify initial filePath is "sub/test.md"
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json buf_data = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data["filePath"].get<std::string>(), std::string("sub/test.md"));
  vxcore_string_free(buffer_json);

  // Save the buffer ID for later comparison
  std::string original_buffer_id(buffer_id);

  // Move file from "sub/test.md" to root
  err = vxcore_node_move(ctx, notebook_id, "sub/test.md", ".");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify buffer's filePath is now "test.md"
  buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  buf_data = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data["filePath"].get<std::string>(), std::string("test.md"));
  vxcore_string_free(buffer_json);

  // Verify buffer ID is unchanged
  ASSERT_EQ(buf_data["id"].get<std::string>(), original_buffer_id);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_move_file_updates_path"));
  std::cout << "  ✓ test_buffer_move_file_updates_path passed" << std::endl;
  return 0;
}

int test_buffer_move_folder_updates_paths() {
  std::cout << "  Running test_buffer_move_folder_updates_paths..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_move_folder_updates_paths"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_move_folder_updates_paths").c_str(),
                               "{\"name\":\"Move Folder Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create folder "src" with two files
  char *src_folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "src", &src_folder_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id1 = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src", "a.md", &file_id1);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id2 = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "src", "b.md", &file_id2);
  ASSERT_EQ(err, VXCORE_OK);

  // Create destination folder "dest"
  char *dest_folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "dest", &dest_folder_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffers for both files
  char *buffer_id1 = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "src/a.md", &buffer_id1);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id2 = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "src/b.md", &buffer_id2);
  ASSERT_EQ(err, VXCORE_OK);

  // Save buffer IDs for later comparison
  std::string original_buffer_id1(buffer_id1);
  std::string original_buffer_id2(buffer_id2);

  // Move folder "src" under "dest"
  err = vxcore_node_move(ctx, notebook_id, "src", "dest");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify first buffer's filePath is "dest/src/a.md"
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id1, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json buf_data1 = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data1["filePath"].get<std::string>(), std::string("dest/src/a.md"));
  vxcore_string_free(buffer_json);

  // Verify second buffer's filePath is "dest/src/b.md"
  buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id2, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json buf_data2 = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data2["filePath"].get<std::string>(), std::string("dest/src/b.md"));
  vxcore_string_free(buffer_json);

  // Verify buffer IDs are unchanged
  ASSERT_EQ(buf_data1["id"].get<std::string>(), original_buffer_id1);
  ASSERT_EQ(buf_data2["id"].get<std::string>(), original_buffer_id2);

  vxcore_string_free(buffer_id1);
  vxcore_string_free(buffer_id2);
  vxcore_string_free(file_id1);
  vxcore_string_free(file_id2);
  vxcore_string_free(src_folder_id);
  vxcore_string_free(dest_folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_move_folder_updates_paths"));
  std::cout << "  ✓ test_buffer_move_folder_updates_paths passed" << std::endl;
  return 0;
}

int test_buffer_move_no_affect_other_buffers() {
  std::cout << "  Running test_buffer_move_no_affect_other_buffers..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_move_no_affect_other"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_move_no_affect_other").c_str(),
                               "{\"name\":\"Move Isolation Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create folder "a" with file "a/file1.md"
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "a", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id1 = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "a", "file1.md", &file_id1);
  ASSERT_EQ(err, VXCORE_OK);

  // Create file in root "file2.md"
  char *file_id2 = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "file2.md", &file_id2);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffers for both
  char *buffer_id1 = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "a/file1.md", &buffer_id1);
  ASSERT_EQ(err, VXCORE_OK);

  char *buffer_id2 = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "file2.md", &buffer_id2);
  ASSERT_EQ(err, VXCORE_OK);

  // Move only file1 from "a/file1.md" to root
  err = vxcore_node_move(ctx, notebook_id, "a/file1.md", ".");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify moved buffer path == "file1.md"
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id1, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json buf_data1 = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data1["filePath"].get<std::string>(), std::string("file1.md"));
  vxcore_string_free(buffer_json);

  // Verify other buffer path == "file2.md" (unchanged)
  buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id2, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json buf_data2 = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data2["filePath"].get<std::string>(), std::string("file2.md"));
  vxcore_string_free(buffer_json);

  vxcore_string_free(buffer_id1);
  vxcore_string_free(buffer_id2);
  vxcore_string_free(file_id1);
  vxcore_string_free(file_id2);
  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_move_no_affect_other"));
  std::cout << "  ✓ test_buffer_move_no_affect_other_buffers passed" << std::endl;
  return 0;
}

int test_buffer_move_provider_functional() {
  std::cout << "  Running test_buffer_move_provider_functional..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_move_provider"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_move_provider").c_str(),
                               "{\"name\":\"Provider Move Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create folder "from" and file "from/note.md"
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "from", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "from", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer for "from/note.md"
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "from/note.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id);

  // Verify initial filePath
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  nlohmann::json buf_data = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data["filePath"].get<std::string>(), std::string("from/note.md"));
  vxcore_string_free(buffer_json);

  // Move file from "from/note.md" to root
  err = vxcore_node_move(ctx, notebook_id, "from/note.md", ".");
  ASSERT_EQ(err, VXCORE_OK);

  // Verify buffer filePath updated
  buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  buf_data = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data["filePath"].get<std::string>(), std::string("note.md"));
  vxcore_string_free(buffer_json);

  // Insert asset after move — proves provider path was updated
  const uint8_t asset_data[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  char *relative_path = nullptr;
  err = vxcore_buffer_insert_asset_raw(ctx, buffer_id, "test.png", asset_data, sizeof(asset_data),
                                       &relative_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(relative_path);

  // Verify asset path format
  std::string rel_path(relative_path);
  ASSERT_TRUE(rel_path.find("vx_assets") != std::string::npos);
  ASSERT_TRUE(rel_path.find("test.png") != std::string::npos);

  // Verify the asset file exists on disk
  std::string notebook_root = get_test_path("test_buffer_move_provider");
  std::string asset_abs_path = notebook_root + "/" + rel_path;
  ASSERT_TRUE(path_exists(asset_abs_path));

  vxcore_string_free(relative_path);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_move_provider"));
  std::cout << "  ✓ test_buffer_move_provider_functional passed" << std::endl;
  return 0;
}

// ============ Buffer Resource Base Path Tests ============

int test_buffer_get_resource_base_path() {
  std::cout << "  Running test_buffer_get_resource_base_path..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_resource_base"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_resource_base").c_str(),
                               "{\"name\":\"Resource Base Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a test file at root level
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Get resource base path
  char *base_path = nullptr;
  err = vxcore_buffer_get_resource_base_path(ctx, buffer_id, &base_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(base_path);

  // For a root-level file, the resource base path should be the notebook root
  std::string base_str = normalize_path(base_path);
  std::string notebook_root = normalize_path(get_test_path("test_buffer_resource_base"));
  ASSERT_EQ(base_str, notebook_root);

  vxcore_string_free(base_path);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_resource_base"));
  std::cout << "  ✓ test_buffer_get_resource_base_path passed" << std::endl;
  return 0;
}

int test_buffer_get_resource_base_path_subfolder() {
  std::cout << "  Running test_buffer_get_resource_base_path_subfolder..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_resource_sub"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("test_buffer_resource_sub").c_str(),
                               "{\"name\":\"Resource Sub Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a subfolder and file
  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "docs", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, "docs", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open buffer
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "docs/note.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Get resource base path
  char *base_path = nullptr;
  err = vxcore_buffer_get_resource_base_path(ctx, buffer_id, &base_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(base_path);

  // For a file in docs/, the resource base path should be notebook_root/docs
  std::string base_str = normalize_path(base_path);
  std::string expected = normalize_path(get_test_path("test_buffer_resource_sub") + "/docs");
  ASSERT_EQ(base_str, expected);

  vxcore_string_free(base_path);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_resource_sub"));
  std::cout << "  ✓ test_buffer_get_resource_base_path_subfolder passed" << std::endl;
  return 0;
}

int test_buffer_get_resource_base_path_external() {
  std::cout << "  Running test_buffer_get_resource_base_path_external..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_resource_ext"));
  create_directory(get_test_path("test_buffer_resource_ext"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Create an external file
  std::string ext_file_path = get_test_path("test_buffer_resource_ext") + "/notes.md";
  write_file(ext_file_path, "# External Notes\n");

  // Open buffer for external file (notebook_id = NULL)
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, nullptr, ext_file_path.c_str(), &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Get resource base path
  char *base_path = nullptr;
  err = vxcore_buffer_get_resource_base_path(ctx, buffer_id, &base_path);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(base_path);

  // For an external file, the resource base path should be the file's parent directory
  std::string base_str = normalize_path(base_path);
  std::string expected = normalize_path(get_test_path("test_buffer_resource_ext"));
  ASSERT_EQ(base_str, expected);

  vxcore_string_free(base_path);
  vxcore_string_free(buffer_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_resource_ext"));
  std::cout << "  ✓ test_buffer_get_resource_base_path_external passed" << std::endl;
  return 0;
}

// ============ Buffer Auto-Resolve Tests ============

int test_buffer_open_auto_resolve_notebook() {
  std::cout << "  Running test_buffer_open_auto_resolve_notebook..." << std::endl;
  cleanup_test_dir(get_test_path("buf_auto_resolve"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("buf_auto_resolve").c_str(),
                               "{\"name\":\"Auto Resolve Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "auto_test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Build absolute path to the file inside the notebook
  std::string abs_path = normalize_path(get_test_path("buf_auto_resolve") + "/auto_test.md");

  // Open buffer with absolute path and NULL notebook_id
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, nullptr, abs_path.c_str(), &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id);
  ASSERT_NE(std::string(buffer_id), std::string(""));

  // Verify buffer was resolved to notebook context
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_json);

  nlohmann::json buf_data = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data["notebookId"].get<std::string>(), std::string(notebook_id));
  ASSERT_EQ(buf_data["filePath"].get<std::string>(), std::string("auto_test.md"));

  vxcore_string_free(buffer_json);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("buf_auto_resolve"));
  std::cout << "  ✓ test_buffer_open_auto_resolve_notebook passed" << std::endl;
  return 0;
}

int test_buffer_open_auto_resolve_dedup() {
  std::cout << "  Running test_buffer_open_auto_resolve_dedup..." << std::endl;
  cleanup_test_dir(get_test_path("buf_auto_dedup"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("buf_auto_dedup").c_str(),
                               "{\"name\":\"Auto Dedup Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "dedup_test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string abs_path = normalize_path(get_test_path("buf_auto_dedup") + "/dedup_test.md");

  // Open buffer via absolute path (nullptr notebook_id)
  char *buffer_id1 = nullptr;
  err = vxcore_buffer_open(ctx, nullptr, abs_path.c_str(), &buffer_id1);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id1);

  // Open buffer via relative path (with notebook_id)
  char *buffer_id2 = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "dedup_test.md", &buffer_id2);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id2);

  // Same buffer — IDs must match
  ASSERT_EQ(std::string(buffer_id1), std::string(buffer_id2));

  vxcore_string_free(buffer_id1);
  vxcore_string_free(buffer_id2);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("buf_auto_dedup"));

  // Reverse order: relative first, then absolute
  cleanup_test_dir(get_test_path("buf_auto_dedup2"));

  err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("buf_auto_dedup2").c_str(),
                               "{\"name\":\"Auto Dedup Test 2\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "dedup_test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Open relative first
  buffer_id1 = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "dedup_test.md", &buffer_id1);
  ASSERT_EQ(err, VXCORE_OK);

  // Open absolute second
  std::string abs_path2 = normalize_path(get_test_path("buf_auto_dedup2") + "/dedup_test.md");
  buffer_id2 = nullptr;
  err = vxcore_buffer_open(ctx, nullptr, abs_path2.c_str(), &buffer_id2);
  ASSERT_EQ(err, VXCORE_OK);

  // Same buffer — IDs must match
  ASSERT_EQ(std::string(buffer_id1), std::string(buffer_id2));

  vxcore_string_free(buffer_id1);
  vxcore_string_free(buffer_id2);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("buf_auto_dedup2"));

  std::cout << "  ✓ test_buffer_open_auto_resolve_dedup passed" << std::endl;
  return 0;
}

int test_buffer_open_auto_resolve_not_in_notebook() {
  std::cout << "  Running test_buffer_open_auto_resolve_not_in_notebook..." << std::endl;
  cleanup_test_dir(get_test_path("buf_auto_outside_nb"));
  cleanup_test_dir(get_test_path("buf_external_outside"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a notebook at one path
  char *notebook_id = nullptr;
  err =
      vxcore_notebook_create(ctx, get_test_path("buf_auto_outside_nb").c_str(),
                             "{\"name\":\"Outside Test\"}", VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create a file outside the notebook
  create_directory(get_test_path("buf_external_outside"));
  std::string outside_path = normalize_path(get_test_path("buf_external_outside") + "/external.md");
  write_file(outside_path, "external content");

  // Open buffer with absolute path to external file, nullptr notebook_id
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, nullptr, outside_path.c_str(), &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id);
  ASSERT_NE(std::string(buffer_id), std::string(""));

  // Verify it's an external buffer (no notebook association)
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_json);

  nlohmann::json buf_data = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data["notebookId"].get<std::string>(), std::string(""));
  // filePath should be the absolute path (normalized)
  ASSERT_EQ(normalize_path(buf_data["filePath"].get<std::string>()), outside_path);

  vxcore_string_free(buffer_json);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("buf_auto_outside_nb"));
  cleanup_test_dir(get_test_path("buf_external_outside"));
  std::cout << "  ✓ test_buffer_open_auto_resolve_not_in_notebook passed" << std::endl;
  return 0;
}

int test_buffer_open_auto_resolve_unindexed_file() {
  std::cout << "  Running test_buffer_open_auto_resolve_unindexed_file..." << std::endl;
  cleanup_test_dir(get_test_path("buf_auto_unindexed"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("buf_auto_unindexed").c_str(),
                               "{\"name\":\"Unindexed Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Write a file directly on disk inside notebook root (NOT through vxcore API)
  write_file(get_test_path("buf_auto_unindexed") + "/unindexed.md", "unindexed content");

  // Build absolute path
  std::string abs_path = normalize_path(get_test_path("buf_auto_unindexed") + "/unindexed.md");

  // Open buffer with absolute path, nullptr notebook_id
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, nullptr, abs_path.c_str(), &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id);

  // Verify buffer was resolved to notebook context with relative path
  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_json);

  nlohmann::json buf_data = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buf_data["notebookId"].get<std::string>(), std::string(notebook_id));
  ASSERT_EQ(buf_data["filePath"].get<std::string>(), std::string("unindexed.md"));

  // Verify content can be read (buffer works despite file being unindexed)
  const void *content_ptr = nullptr;
  size_t content_size = 0;
  err = vxcore_buffer_get_content_raw(ctx, buffer_id, &content_ptr, &content_size);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(content_ptr);
  std::string retrieved(static_cast<const char *>(content_ptr), content_size);
  ASSERT_EQ(retrieved, std::string("unindexed content"));

  vxcore_string_free(buffer_json);
  vxcore_string_free(buffer_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("buf_auto_unindexed"));
  std::cout << "  ✓ test_buffer_open_auto_resolve_unindexed_file passed" << std::endl;
  return 0;
}

int test_buffer_open_by_node_id() {
  std::cout << "  Running test_buffer_open_by_node_id..." << std::endl;
  cleanup_test_dir(get_test_path("buf_open_by_node_id"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  err = vxcore_notebook_create(ctx, get_test_path("buf_open_by_node_id").c_str(),
                               "{\"name\":\"Open By Node ID Test\"}", VXCORE_NOTEBOOK_BUNDLED,
                               &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "node_id_test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *folder_id = nullptr;
  err = vxcore_folder_create(ctx, notebook_id, ".", "folder_node", &folder_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Happy path.
  char *buffer_id = nullptr;
  err = vxcore_buffer_open_by_node_id(ctx, file_id, &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id);

  char *buffer_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buffer_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_json);

  nlohmann::json buffer_data = nlohmann::json::parse(buffer_json);
  ASSERT_EQ(buffer_data["notebookId"].get<std::string>(), std::string(notebook_id));
  ASSERT_EQ(buffer_data["filePath"].get<std::string>(), std::string("node_id_test.md"));

  // Not found.
  char *missing_buffer_id = nullptr;
  err = vxcore_buffer_open_by_node_id(ctx, "nonexistent-uuid", &missing_buffer_id);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(missing_buffer_id);

  // Folder UUID should not open as a buffer.
  err = vxcore_buffer_open_by_node_id(ctx, folder_id, &missing_buffer_id);
  ASSERT_EQ(err, VXCORE_ERR_NOT_FOUND);
  ASSERT_NULL(missing_buffer_id);

  // Null pointer args.
  err = vxcore_buffer_open_by_node_id(nullptr, file_id, &missing_buffer_id);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  err = vxcore_buffer_open_by_node_id(ctx, nullptr, &missing_buffer_id);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  err = vxcore_buffer_open_by_node_id(ctx, file_id, nullptr);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  // Dedup.
  char *buffer_id2 = nullptr;
  err = vxcore_buffer_open_by_node_id(ctx, file_id, &buffer_id2);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(buffer_id2);
  ASSERT_EQ(std::string(buffer_id), std::string(buffer_id2));

  // Not initialized.
  VxCoreContextHandle ctx_no_notebook = nullptr;
  err = vxcore_context_create(nullptr, &ctx_no_notebook);
  ASSERT_EQ(err, VXCORE_OK);
  auto *vctx_no_notebook = reinterpret_cast<vxcore::VxCoreContext *>(ctx_no_notebook);
  (void)vctx_no_notebook->notebook_manager.release();  // leak ok in test

  char *uninitialized_buffer_id = reinterpret_cast<char *>(1);
  err = vxcore_buffer_open_by_node_id(ctx_no_notebook, file_id, &uninitialized_buffer_id);
  ASSERT_EQ(err, VXCORE_ERR_NOT_INITIALIZED);
  ASSERT_NULL(uninitialized_buffer_id);
  vxcore_context_destroy(ctx_no_notebook);

  VxCoreContextHandle ctx_no_buffer = nullptr;
  err = vxcore_context_create(nullptr, &ctx_no_buffer);
  ASSERT_EQ(err, VXCORE_OK);
  auto *vctx_no_buffer = reinterpret_cast<vxcore::VxCoreContext *>(ctx_no_buffer);
  (void)vctx_no_buffer->buffer_manager.release();  // leak ok in test

  uninitialized_buffer_id = reinterpret_cast<char *>(1);
  err = vxcore_buffer_open_by_node_id(ctx_no_buffer, file_id, &uninitialized_buffer_id);
  ASSERT_EQ(err, VXCORE_ERR_NOT_INITIALIZED);
  ASSERT_NULL(uninitialized_buffer_id);
  vxcore_context_destroy(ctx_no_buffer);

  vxcore_string_free(buffer_id2);
  vxcore_string_free(buffer_json);
  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(folder_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("buf_open_by_node_id"));
  std::cout << "  ✓ test_buffer_open_by_node_id passed" << std::endl;
  return 0;
}

int test_virtual_buffer_open() {
  std::cout << "  Running test_virtual_buffer_open..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *id = nullptr;
  err = vxcore_buffer_open_virtual(ctx, "vx://settings", &id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(id);

  char *list_json = nullptr;
  err = vxcore_buffer_list(ctx, &list_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(list_json);

  nlohmann::json buffer_list = nlohmann::json::parse(list_json);
  ASSERT(buffer_list.is_array());

  bool found = false;
  for (const auto &buf : buffer_list) {
    if (buf.contains("id") && buf["id"].is_string() && buf["id"].get<std::string>() == id) {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);

  vxcore_string_free(list_json);
  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_virtual_buffer_open passed" << std::endl;
  return 0;
}

int test_virtual_buffer_save_noop() {
  std::cout << "  Running test_virtual_buffer_save_noop..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *id = nullptr;
  err = vxcore_buffer_open_virtual(ctx, "vx://settings", &id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(id);

  err = vxcore_buffer_save(ctx, id);
  ASSERT_EQ(err, VXCORE_OK);

  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_virtual_buffer_save_noop passed" << std::endl;
  return 0;
}

int test_virtual_buffer_content_empty() {
  std::cout << "  Running test_virtual_buffer_content_empty..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *id = nullptr;
  err = vxcore_buffer_open_virtual(ctx, "vx://settings", &id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(id);

  char *content_json = nullptr;
  err = vxcore_buffer_get_content(ctx, id, &content_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(content_json);

  nlohmann::json content_data = nlohmann::json::parse(content_json);
  ASSERT(content_data.contains("content"));
  std::vector<uint8_t> decoded = test_base64_decode(content_data["content"].get<std::string>());
  ASSERT_TRUE(decoded.empty());

  vxcore_string_free(content_json);
  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_virtual_buffer_content_empty passed" << std::endl;
  return 0;
}

int test_virtual_buffer_state_normal() {
  std::cout << "  Running test_virtual_buffer_state_normal..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *id = nullptr;
  err = vxcore_buffer_open_virtual(ctx, "vx://settings", &id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(id);

  char *json = nullptr;
  err = vxcore_buffer_get(ctx, id, &json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(json);

  nlohmann::json buffer_data = nlohmann::json::parse(json);
  ASSERT(buffer_data.contains("state"));
  ASSERT_EQ(buffer_data["state"].get<int>(), 0);

  vxcore_string_free(json);
  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_virtual_buffer_state_normal passed" << std::endl;
  return 0;
}

int test_virtual_buffer_close() {
  std::cout << "  Running test_virtual_buffer_close..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *id = nullptr;
  err = vxcore_buffer_open_virtual(ctx, "vx://settings", &id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(id);

  err = vxcore_buffer_close(ctx, id);
  ASSERT_EQ(err, VXCORE_OK);

  char *list_json = nullptr;
  err = vxcore_buffer_list(ctx, &list_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(list_json);

  nlohmann::json buffer_list = nlohmann::json::parse(list_json);
  ASSERT(buffer_list.is_array());
  for (const auto &buf : buffer_list) {
    if (buf.contains("id") && buf["id"].is_string()) {
      ASSERT_NE(buf["id"].get<std::string>(), std::string(id));
    }
  }

  vxcore_string_free(list_json);
  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_virtual_buffer_close passed" << std::endl;
  return 0;
}

int test_virtual_buffer_dedup() {
  std::cout << "  Running test_virtual_buffer_dedup..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *id1 = nullptr;
  char *id2 = nullptr;
  err = vxcore_buffer_open_virtual(ctx, "vx://settings", &id1);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(id1);
  err = vxcore_buffer_open_virtual(ctx, "vx://settings", &id2);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(id2);
  ASSERT_EQ(std::string(id1), std::string(id2));

  vxcore_string_free(id2);
  vxcore_string_free(id1);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_virtual_buffer_dedup passed" << std::endl;
  return 0;
}

int test_virtual_buffer_not_in_session() {
  std::cout << "  Running test_virtual_buffer_not_in_session..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *id = nullptr;
  err = vxcore_buffer_open_virtual(ctx, "vx://settings", &id);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(id);

  char *session_json = nullptr;
  err = vxcore_context_get_session_config(ctx, &session_json);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(session_json);

  nlohmann::json session = nlohmann::json::parse(session_json);
  if (session.contains("buffers") && session["buffers"].is_array()) {
    for (const auto &buf : session["buffers"]) {
      if (buf.is_object()) {
        ASSERT_NE(buf.value("id", ""), std::string(id));
      }
    }
  }

  vxcore_string_free(session_json);
  vxcore_string_free(id);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_virtual_buffer_not_in_session passed" << std::endl;
  return 0;
}

int test_virtual_buffer_rejected_by_regular_open() {
  std::cout << "  Running test_virtual_buffer_rejected_by_regular_open..." << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *id = nullptr;
  err = vxcore_buffer_open(ctx, "", "vx://settings", &id);
  ASSERT_NE(err, VXCORE_OK);

  if (id) {
    vxcore_string_free(id);
  }
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_virtual_buffer_rejected_by_regular_open passed" << std::endl;
  return 0;
}

int test_buffer_check_external_changes() {
  std::cout << "  Running test_buffer_check_external_changes..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_ext_changes"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_ext_changes");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"ExtChange Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create file and open buffer
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "test.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);

  std::string file_path = notebook_path + "/test.md";
  create_test_file(file_path, "Initial content");

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "test.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Reload to establish baseline (file_create + create_test_file may cause mismatch)
  err = vxcore_buffer_reload(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify state is NORMAL after reload
  VxCoreBufferState state;
  err = vxcore_buffer_get_state(ctx, buffer_id, &state);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_NORMAL);

  // Check external changes when nothing changed - should stay NORMAL
  err = vxcore_buffer_check_external_changes(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_get_state(ctx, buffer_id, &state);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_NORMAL);

  // Modify file externally (sleep for mtime granularity)
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  create_test_file(file_path, "Externally modified content");

  // Check external changes - should detect FILE_CHANGED
  err = vxcore_buffer_check_external_changes(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_get_state(ctx, buffer_id, &state);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_FILE_CHANGED);

  // Reload buffer - should reset to NORMAL
  err = vxcore_buffer_reload(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_get_state(ctx, buffer_id, &state);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_NORMAL);

  // Delete file externally
  std::filesystem::remove(std::filesystem::path(file_path));

  // Check external changes - should detect FILE_MISSING
  err = vxcore_buffer_check_external_changes(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_get_state(ctx, buffer_id, &state);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_FILE_MISSING);

  // Test error cases
  err = vxcore_buffer_check_external_changes(nullptr, buffer_id);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  err = vxcore_buffer_check_external_changes(ctx, nullptr);
  ASSERT_EQ(err, VXCORE_ERR_NULL_POINTER);

  err = vxcore_buffer_check_external_changes(ctx, "nonexistent-id");
  ASSERT_EQ(err, VXCORE_ERR_BUFFER_NOT_FOUND);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_ext_changes"));
  std::cout << "  ✓ test_buffer_check_external_changes passed" << std::endl;
  return 0;
}

int test_buffer_check_external_changes_unloaded() {
  std::cout << "  Running test_buffer_check_external_changes_unloaded..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_ext_unloaded"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_ext_unloaded");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Unloaded Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Create file with content on disk
  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "lazy.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string file_path = notebook_path + "/lazy.md";
  create_test_file(file_path, "Lazy content");

  // Open buffer (lazy — DO NOT read content or reload)
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "lazy.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Verify content_loaded == false (buffer not yet loaded from disk)
  char *buf_json = nullptr;
  err = vxcore_buffer_get(ctx, buffer_id, &buf_json);
  ASSERT_EQ(err, VXCORE_OK);
  auto j = nlohmann::json::parse(buf_json);
  ASSERT_FALSE(j.value("contentLoaded", true));
  vxcore_string_free(buf_json);

  // Check external changes — should NOT detect FILE_CHANGED for unloaded buffer
  err = vxcore_buffer_check_external_changes(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  VxCoreBufferState state;
  err = vxcore_buffer_get_state(ctx, buffer_id, &state);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_NORMAL);  // Bug 1 fix: NOT FILE_CHANGED

  // Now trigger lazy load by reading content
  const void *data = nullptr;
  size_t size = 0;
  err = vxcore_buffer_get_content_raw(ctx, buffer_id, &data, &size);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_TRUE(size > 0);

  // Verify content_loaded == true now
  err = vxcore_buffer_get(ctx, buffer_id, &buf_json);
  ASSERT_EQ(err, VXCORE_OK);
  j = nlohmann::json::parse(buf_json);
  ASSERT_TRUE(j.value("contentLoaded", false));
  vxcore_string_free(buf_json);

  // Modify file on disk
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  create_test_file(file_path, "Externally modified lazy content");

  // Check external changes — SHOULD detect FILE_CHANGED now (content was loaded)
  err = vxcore_buffer_check_external_changes(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_get_state(ctx, buffer_id, &state);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_FILE_CHANGED);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_ext_unloaded"));
  std::cout << "  ✓ test_buffer_check_external_changes_unloaded passed" << std::endl;
  return 0;
}

int test_buffer_mtime_no_jitter() {
  std::cout << "  Running test_buffer_mtime_no_jitter..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_mtime_jitter"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_mtime_jitter");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Jitter Test\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "stable.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string file_path = notebook_path + "/stable.md";
  create_test_file(file_path, "Stable content that never changes");

  // Open buffer and load content (establishes baseline mtime)
  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "stable.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Trigger content load
  const void *data = nullptr;
  size_t size = 0;
  err = vxcore_buffer_get_content_raw(ctx, buffer_id, &data, &size);
  ASSERT_EQ(err, VXCORE_OK);

  // Check external changes 100 times — should NEVER detect FILE_CHANGED
  for (int i = 0; i < 100; i++) {
    err = vxcore_buffer_check_external_changes(ctx, buffer_id);
    ASSERT_EQ(err, VXCORE_OK);
    VxCoreBufferState state;
    err = vxcore_buffer_get_state(ctx, buffer_id, &state);
    ASSERT_EQ(err, VXCORE_OK);
    if (state != VXCORE_BUFFER_NORMAL) {
      std::cerr << "  FAIL: Jitter detected at iteration " << i << " state=" << state << std::endl;
      return 1;
    }
  }

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_mtime_jitter"));
  std::cout << "  ✓ test_buffer_mtime_no_jitter passed" << std::endl;
  return 0;
}

// Content-aware external-change detection: a bare mtime bump with identical
// (or EOL-only-different) content must NOT be reported as FILE_CHANGED.
int test_buffer_external_change_content_aware() {
  std::cout << "  Running test_buffer_external_change_content_aware..." << std::endl;
  cleanup_test_dir(get_test_path("test_buffer_ext_content"));

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);

  char *notebook_id = nullptr;
  std::string notebook_path = get_test_path("test_buffer_ext_content");
  err = vxcore_notebook_create(ctx, notebook_path.c_str(), "{\"name\":\"Content Aware\"}",
                               VXCORE_NOTEBOOK_BUNDLED, &notebook_id);
  ASSERT_EQ(err, VXCORE_OK);

  char *file_id = nullptr;
  err = vxcore_file_create(ctx, notebook_id, ".", "note.md", &file_id);
  ASSERT_EQ(err, VXCORE_OK);
  std::string file_path = notebook_path + "/note.md";
  create_test_file(file_path, "line1\nline2\nline3");

  char *buffer_id = nullptr;
  err = vxcore_buffer_open(ctx, notebook_id, "note.md", &buffer_id);
  ASSERT_EQ(err, VXCORE_OK);

  // Establish baseline (content_ loaded, last_modified_time_ stamped).
  err = vxcore_buffer_reload(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  VxCoreBufferState state;
  err = vxcore_buffer_get_state(ctx, buffer_id, &state);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_NORMAL);

  // (b) Rewrite the SAME content with a fresh mtime -> benign, stays NORMAL.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  create_test_file(file_path, "line1\nline2\nline3");
  err = vxcore_buffer_check_external_changes(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_get_state(ctx, buffer_id, &state);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_NORMAL);

  // Stamp must have been refreshed: a second check sees no change, stays NORMAL.
  err = vxcore_buffer_check_external_changes(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_get_state(ctx, buffer_id, &state);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_NORMAL);

  // (c)/(e) Rewrite with CRLF line endings (different bytes AND different size)
  // but identical logical content -> benign, stays NORMAL.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  create_test_file(file_path, "line1\r\nline2\r\nline3");
  err = vxcore_buffer_check_external_changes(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_get_state(ctx, buffer_id, &state);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_NORMAL);

  // (d) Rewrite with genuinely different content -> FILE_CHANGED.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  create_test_file(file_path, "line1\nTOTALLY DIFFERENT\nline3");
  err = vxcore_buffer_check_external_changes(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_get_state(ctx, buffer_id, &state);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_FILE_CHANGED);

  // A real change must keep flagging (stamp NOT refreshed) until resolved.
  err = vxcore_buffer_check_external_changes(ctx, buffer_id);
  ASSERT_EQ(err, VXCORE_OK);
  err = vxcore_buffer_get_state(ctx, buffer_id, &state);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_EQ(state, VXCORE_BUFFER_FILE_CHANGED);

  vxcore_string_free(buffer_id);
  vxcore_string_free(file_id);
  vxcore_string_free(notebook_id);
  vxcore_context_destroy(ctx);
  cleanup_test_dir(get_test_path("test_buffer_ext_content"));
  std::cout << "  ✓ test_buffer_external_change_content_aware passed" << std::endl;
  return 0;
}

int test_encryption_prepare_abandon() {
  EncryptionFixture fixture;
  ASSERT_EQ(fixture.error, VXCORE_OK);
  const auto before = encryption_file_tree(fixture.path);
  const auto context = fixture.context.value;
  const auto notebook_id = fixture.notebook_id.value;
  ASSERT(encryption_status(context, notebook_id) == expected_encryption_status(false, false));

  VxCoreEncryptionSetupHandle setup = nullptr;
  ASSERT_EQ(vxcore_encryption_prepare_notebook(context, notebook_id, nullptr, "", 0, &setup),
            VXCORE_ERR_INVALID_PARAM);
  ASSERT_NULL(setup);
  ASSERT_EQ(vxcore_encryption_prepare_notebook(context, notebook_id, nullptr,
      kEncryptionPassword.data(), kEncryptionPassword.size(), &setup), VXCORE_OK);
  ASSERT_NOT_NULL(setup);
  ASSERT(encryption_file_tree(fixture.path) == before);
  ASSERT_EQ(vxcore_encryption_lock_all(context), VXCORE_ERR_INVALID_STATE);
  ASSERT(encryption_status(context, notebook_id) == expected_encryption_status(false, false));

  vxcore_encryption_free_setup(context, setup);
  vxcore_encryption_free_setup(context, nullptr);
  ASSERT_EQ(vxcore_encryption_lock_all(context), VXCORE_OK);
  ASSERT(encryption_file_tree(fixture.path) == before);
  ASSERT_EQ(vxcore_encryption_commit_notebook(context, setup), VXCORE_ERR_INVALID_PARAM);
  return 0;
}

int test_encryption_commit_and_existing_key() {
  EncryptionFixture fixture;
  ASSERT_EQ(fixture.error, VXCORE_OK);
  const auto context = fixture.context.value;
  const auto notebook_id = fixture.notebook_id.value;
  EncryptionTestGitRepository repository;
  ASSERT_EQ(git_repository_init(&repository.value, fixture.path.c_str(), 0), 0);
  // A formerly correct rule followed by an override must not fool initialization.
  const std::string original_attributes =
      "# Keep user attributes\r\n*.md eol=lf\r\n*.vne -text -diff -merge\r\n"
      "*.vne text diff merge\r\n*.custom filter=keep";
  const auto attributes_path = fixture.path + "/.gitattributes";
  write_file(attributes_path, original_attributes);
  const auto before = encryption_file_tree(fixture.path);
  VxCoreEncryptionSetupHandle first = nullptr;
  VxCoreEncryptionSetupHandle competing = nullptr;
  ASSERT_EQ(vxcore_encryption_prepare_notebook(context, notebook_id, nullptr,
      kEncryptionPassword.data(), kEncryptionPassword.size(), &first), VXCORE_OK);
  ASSERT_EQ(vxcore_encryption_prepare_notebook(context, notebook_id, nullptr,
      kEncryptionPassword.data(), kEncryptionPassword.size(), &competing), VXCORE_OK);
  ASSERT(encryption_file_tree(fixture.path) == before);
  ASSERT_EQ(vxcore_encryption_commit_notebook(context, first), VXCORE_OK);
  const auto key_bytes = read_file_content(fixture.key_path());
  ASSERT_EQ(assert_key_envelope(key_bytes, notebook_id), 0);
  const auto status = encryption_status(context, notebook_id);
  const auto vault_id = nlohmann::json::parse(key_bytes.substr(12))["vaultId"].get<std::string>();
  ASSERT(status == expected_encryption_status(true, true, vault_id));
  ASSERT_EQ(read_file_content(attributes_path).substr(0, original_attributes.size()),
            original_attributes);
  ASSERT_EQ(assert_binary_encryption_attributes(repository.value), 0);

  ASSERT_EQ(vxcore_encryption_commit_notebook(context, first), VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_encryption_lock_all(context), VXCORE_ERR_INVALID_STATE);
  const auto committed = encryption_file_tree(fixture.path);
  ASSERT_EQ(vxcore_encryption_commit_notebook(context, competing), VXCORE_ERR_ALREADY_EXISTS);
  ASSERT_EQ(vxcore_encryption_commit_notebook(context, competing), VXCORE_ERR_INVALID_PARAM);
  ASSERT(encryption_file_tree(fixture.path) == committed);
  ASSERT(encryption_status(context, notebook_id) == status);
  VxCoreEncryptionSetupHandle replacement = nullptr;
  ASSERT_EQ(vxcore_encryption_prepare_notebook(context, notebook_id, nullptr,
      kEncryptionPassword.data(), kEncryptionPassword.size(), &replacement),
      VXCORE_ERR_ALREADY_EXISTS);
  ASSERT_NULL(replacement);
  ASSERT(encryption_file_tree(fixture.path) == committed);
  ASSERT_EQ(vxcore_encryption_lock_all(context), VXCORE_OK);
  ASSERT(encryption_status(context, notebook_id) == expected_encryption_status(true, false, vault_id));
  return 0;
}

int test_encryption_setup_context_ownership() {
  EncryptionFixture fixture;
  ASSERT_EQ(fixture.error, VXCORE_OK);
  const auto context = fixture.context.value;
  const auto notebook_id = fixture.notebook_id.value;
  const auto before = encryption_file_tree(fixture.path);
  EncryptionTestContext other;
  ASSERT_EQ(vxcore_context_create(nullptr, &other.value), VXCORE_OK);
  VxCoreEncryptionSetupHandle setup = nullptr;
  ASSERT_EQ(vxcore_encryption_prepare_notebook(context, notebook_id, nullptr,
      kEncryptionPassword.data(), kEncryptionPassword.size(), &setup), VXCORE_OK);
  ASSERT_EQ(vxcore_encryption_commit_notebook(other.value, setup), VXCORE_ERR_INVALID_PARAM);
  vxcore_encryption_free_setup(other.value, setup);
  ASSERT_EQ(vxcore_encryption_lock_all(other.value), VXCORE_OK);
  ASSERT_EQ(vxcore_encryption_lock_all(context), VXCORE_ERR_INVALID_STATE);
  ASSERT(encryption_file_tree(fixture.path) == before);
  ASSERT_EQ(vxcore_encryption_commit_notebook(context, setup), VXCORE_OK);
  ASSERT_EQ(vxcore_encryption_lock_all(context), VXCORE_OK);

  EncryptionTestString abandoned_id;
  const auto abandoned_path = get_test_path("abandoned");
  ASSERT_EQ(vxcore_notebook_create(other.value, abandoned_path.c_str(), "{\"name\":\"Abandoned\"}",
      VXCORE_NOTEBOOK_BUNDLED, &abandoned_id.value), VXCORE_OK);
  const auto abandoned_before = encryption_file_tree(abandoned_path);
  ASSERT_EQ(vxcore_encryption_prepare_notebook(other.value, abandoned_id.value, nullptr,
      kEncryptionPassword.data(), kEncryptionPassword.size(), &setup), VXCORE_OK);
  vxcore_context_destroy(other.value);
  other.value = nullptr;
  ASSERT(encryption_file_tree(abandoned_path) == abandoned_before);
  ASSERT_EQ(vxcore_context_create(nullptr, &other.value), VXCORE_OK);
  EncryptionTestString reopened_id;
  ASSERT_EQ(vxcore_notebook_open(other.value, abandoned_path.c_str(), &reopened_id.value), VXCORE_OK);
  ASSERT_EQ(vxcore_encryption_lock_all(other.value), VXCORE_OK);
  ASSERT(encryption_status(other.value, reopened_id.value) == expected_encryption_status(false, false));
  return 0;
}

int test_encryption_read_only_transitions() {
  EncryptionFixture fixture;
  ASSERT_EQ(fixture.error, VXCORE_OK);
  const auto context = fixture.context.value;
  const auto notebook_id = fixture.notebook_id.value;
  const auto before = encryption_file_tree(fixture.path);
  ASSERT_EQ(vxcore_notebook_set_read_only(context, notebook_id, true), VXCORE_OK);
  VxCoreEncryptionSetupHandle setup = nullptr;
  ASSERT_EQ(vxcore_encryption_prepare_notebook(context, notebook_id, nullptr,
      kEncryptionPassword.data(), kEncryptionPassword.size(), &setup), VXCORE_ERR_READ_ONLY);
  ASSERT_NULL(setup);
  ASSERT(encryption_file_tree(fixture.path) == before);

  ASSERT_EQ(vxcore_notebook_set_read_only(context, notebook_id, false), VXCORE_OK);
  ASSERT_EQ(vxcore_encryption_prepare_notebook(context, notebook_id, nullptr,
      kEncryptionPassword.data(), kEncryptionPassword.size(), &setup), VXCORE_OK);
  ASSERT_EQ(vxcore_notebook_set_read_only(context, notebook_id, true), VXCORE_OK);
  ASSERT_EQ(vxcore_encryption_commit_notebook(context, setup), VXCORE_ERR_READ_ONLY);
  ASSERT_EQ(vxcore_encryption_commit_notebook(context, setup), VXCORE_ERR_INVALID_PARAM);
  ASSERT_EQ(vxcore_encryption_lock_all(context), VXCORE_OK);
  ASSERT(encryption_file_tree(fixture.path) == before);

  ASSERT_EQ(vxcore_notebook_set_read_only(context, notebook_id, false), VXCORE_OK);
  ASSERT_EQ(initialize_test_encryption(context, notebook_id), VXCORE_OK);
  const auto unlocked = encryption_status(context, notebook_id);
  ASSERT_EQ(vxcore_notebook_set_read_only(context, notebook_id, true), VXCORE_OK);
  ASSERT_EQ(vxcore_encryption_lock_all(context), VXCORE_OK);
  const auto read_only_files = encryption_file_tree(fixture.path);
  ASSERT_EQ(vxcore_encryption_unlock_notebook(context, notebook_id,
      kEncryptionPassword.data(), kEncryptionPassword.size()), VXCORE_OK);
  ASSERT(encryption_status(context, notebook_id) == unlocked);
  ASSERT(encryption_file_tree(fixture.path) == read_only_files);
  return 0;
}

int test_encryption_failed_unlock_preserves_state() {
  EncryptionFixture fixture;
  ASSERT_EQ(fixture.error, VXCORE_OK);
  const auto context = fixture.context.value;
  const auto notebook_id = fixture.notebook_id.value;
  ASSERT_EQ(initialize_test_encryption(context, notebook_id), VXCORE_OK);
  EncryptionTestString sibling_id;
  const auto sibling_path = get_test_path("sibling");
  ASSERT_EQ(vxcore_notebook_create(context, sibling_path.c_str(), "{\"name\":\"Sibling\"}",
      VXCORE_NOTEBOOK_BUNDLED, &sibling_id.value), VXCORE_OK);
  ASSERT_EQ(initialize_test_encryption(context, sibling_id.value, notebook_id), VXCORE_OK);
  const auto original = read_file_content(fixture.key_path());
  const auto unlocked = encryption_status(context, notebook_id);
  const auto sibling_unlocked = encryption_status(context, sibling_id.value);
  ASSERT_EQ(vxcore_encryption_lock_all(context), VXCORE_OK);
  const auto locked = encryption_status(context, notebook_id);
  const std::string wrong_password = "incorrect password";
  ASSERT_EQ(vxcore_encryption_unlock_notebook(context, notebook_id,
      wrong_password.data(), wrong_password.size()), VXCORE_ERR_ENCRYPTION_AUTH_FAILED);
  ASSERT(encryption_status(context, notebook_id) == locked);
  ASSERT_EQ(read_file_content(fixture.key_path()), original);
  ASSERT_EQ(vxcore_encryption_unlock_notebook(context, notebook_id,
      kEncryptionPassword.data(), kEncryptionPassword.size()), VXCORE_OK);
  ASSERT_EQ(vxcore_encryption_unlock_notebook(context, sibling_id.value,
      kEncryptionPassword.data(), kEncryptionPassword.size()), VXCORE_OK);
  ASSERT_EQ(vxcore_encryption_unlock_notebook(context, notebook_id,
      wrong_password.data(), wrong_password.size()), VXCORE_ERR_ENCRYPTION_AUTH_FAILED);
  ASSERT(encryption_status(context, notebook_id) == unlocked);
  ASSERT(encryption_status(context, sibling_id.value) == sibling_unlocked);

  auto damaged = nlohmann::json::parse(original.substr(12));
  auto wrapped_key = damaged["wrappedNotebookKey"].get<std::string>();
  wrapped_key[0] = wrapped_key[0] == 'A' ? 'B' : 'A';
  damaged["wrappedNotebookKey"] = wrapped_key;
  const auto tampered = encryption_key_file(damaged.dump());
  write_file(fixture.key_path(), tampered);
  ASSERT_EQ(vxcore_encryption_unlock_notebook(context, notebook_id,
      kEncryptionPassword.data(), kEncryptionPassword.size()), VXCORE_ERR_ENCRYPTION_AUTH_FAILED);
  ASSERT_EQ(read_file_content(fixture.key_path()), tampered);
  write_file(fixture.key_path(), original);
  ASSERT(encryption_status(context, notebook_id) == unlocked);
  ASSERT(encryption_status(context, sibling_id.value) == sibling_unlocked);
  return 0;
}

int test_encryption_rejects_malformed_key_files() {
  EncryptionFixture fixture;
  ASSERT_EQ(fixture.error, VXCORE_OK);
  const auto context = fixture.context.value;
  const auto notebook_id = fixture.notebook_id.value;
  ASSERT_EQ(initialize_test_encryption(context, notebook_id), VXCORE_OK);
  ASSERT_EQ(vxcore_encryption_lock_all(context), VXCORE_OK);
  const auto original = read_file_content(fixture.key_path());
  const auto locked = encryption_status(context, notebook_id);
  const auto valid = nlohmann::json::parse(original.substr(12));
  std::vector<std::string> malformed;
  auto unknown = valid;
  unknown["plaintextKey"] = "must not be accepted";
  malformed.push_back(encryption_key_file(unknown.dump()));
  auto duplicate = valid.dump();
  duplicate.insert(duplicate.size() - 1, ",\"version\":1");
  malformed.push_back(encryption_key_file(duplicate));
  auto excessive_kdf = valid;
  excessive_kdf["memlimit"] = 1073741824;
  malformed.push_back(encryption_key_file(excessive_kdf.dump()));
  auto noncanonical_base64 = valid;
  noncanonical_base64["salt"] = valid["salt"].get<std::string>() + "\n";
  malformed.push_back(encryption_key_file(noncanonical_base64.dump()));
  malformed.push_back(encryption_key_file(std::string(4097, ' ')));
  malformed.push_back(original.substr(0, original.size() - 1));
  for (const auto &bytes : malformed) {
    write_file(fixture.key_path(), bytes);
    ASSERT_EQ(vxcore_encryption_unlock_notebook(context, notebook_id,
        kEncryptionPassword.data(), kEncryptionPassword.size()), VXCORE_ERR_ENCRYPTION_FORMAT);
    EncryptionTestString status;
    ASSERT_EQ(vxcore_encryption_get_status(context, notebook_id, nullptr, &status.value),
              VXCORE_ERR_ENCRYPTION_FORMAT);
    ASSERT_NULL(status.value);
    ASSERT_EQ(read_file_content(fixture.key_path()), bytes);
    write_file(fixture.key_path(), original);
    ASSERT(encryption_status(context, notebook_id) == locked);
  }
  ASSERT_EQ(vxcore_encryption_unlock_notebook(context, notebook_id,
      kEncryptionPassword.data(), kEncryptionPassword.size()), VXCORE_OK);
  return 0;
}

int test_encryption_notebook_copy_portability() {
  // The copied notebook is outside device A's disposable profile/notebook root.
  const auto copied_path = get_test_path("portable_notebook_copy");
  std::string device_a_path;
  std::string expected_id;
  std::string vault_id;
  std::string copied_key;
  {
    EncryptionFixture device_a;
    ASSERT_EQ(device_a.error, VXCORE_OK);
    device_a_path = fs_path_to_utf8(device_a.directory.path());
    const auto context = device_a.context.value;
    ASSERT_EQ(initialize_test_encryption(context, device_a.notebook_id.value), VXCORE_OK);
    EncryptionTestString second_id;
    const auto second_path = get_test_path("second_notebook");
    ASSERT_EQ(vxcore_notebook_create(context, second_path.c_str(), "{\"name\":\"Second\"}",
        VXCORE_NOTEBOOK_BUNDLED, &second_id.value), VXCORE_OK);
    ASSERT_EQ(initialize_test_encryption(context, second_id.value, device_a.notebook_id.value),
              VXCORE_OK);
    const auto first_key = nlohmann::json::parse(read_file_content(device_a.key_path()).substr(12));
    copied_key = read_file_content(second_path + "/vx_notebook/encryption.vne");
    const auto second_key = nlohmann::json::parse(copied_key.substr(12));
    // Source setup reuses its password envelope, not its notebook identity/key envelope.
    for (const auto *field : {"vaultId", "kdf", "opslimit", "memlimit", "salt", "masterNonce",
                              "wrappedMasterKey"}) {
      ASSERT(first_key.at(field) == second_key.at(field));
    }
    ASSERT(first_key.at("notebookKeyId") != second_key.at("notebookKeyId"));
    ASSERT(first_key.at("wrappedNotebookKey") != second_key.at("wrappedNotebookKey"));
    expected_id = second_id.value;
    vault_id = second_key.at("vaultId").get<std::string>();
    ASSERT_EQ(vxcore_notebook_close(context, second_id.value), VXCORE_OK);
    std::filesystem::create_directories(utf8_to_fs_path(copied_path).parent_path());
    std::filesystem::copy(utf8_to_fs_path(second_path), utf8_to_fs_path(copied_path),
                          std::filesystem::copy_options::recursive);
  }
  ASSERT_FALSE(path_exists(device_a_path));
  {
    BufferTestTempDirectory device_b_directory;
    ASSERT_TRUE(device_b_directory.valid());
    EncryptionTestContext device_b;
    ASSERT_EQ(vxcore_context_create(nullptr, &device_b.value), VXCORE_OK);
    EncryptionTestString notebook_id;
    ASSERT_EQ(vxcore_notebook_open(device_b.value, copied_path.c_str(), &notebook_id.value), VXCORE_OK);
    ASSERT_EQ(std::string(notebook_id.value), expected_id);
    ASSERT(encryption_status(device_b.value, notebook_id.value) ==
           expected_encryption_status(true, false, vault_id));
    // Embedded NUL, Unicode, and surrounding spaces are exact password bytes.
    const std::string truncated_password(kEncryptionPassword.c_str());
    ASSERT_EQ(vxcore_encryption_unlock_notebook(device_b.value, notebook_id.value,
        truncated_password.data(), truncated_password.size()), VXCORE_ERR_ENCRYPTION_AUTH_FAILED);
    const auto trimmed_password = kEncryptionPassword.substr(2, kEncryptionPassword.size() - 4);
    ASSERT_EQ(vxcore_encryption_unlock_notebook(device_b.value, notebook_id.value,
        trimmed_password.data(), trimmed_password.size()), VXCORE_ERR_ENCRYPTION_AUTH_FAILED);
    ASSERT_EQ(vxcore_encryption_unlock_notebook(device_b.value, notebook_id.value,
        kEncryptionPassword.data(), kEncryptionPassword.size()), VXCORE_OK);
    ASSERT(encryption_status(device_b.value, notebook_id.value) ==
           expected_encryption_status(true, true, vault_id));
    ASSERT_EQ(read_file_content(copied_path + "/vx_notebook/encryption.vne"), copied_key);
    ASSERT_EQ(vxcore_encryption_lock_all(device_b.value), VXCORE_OK);
  }
  cleanup_test_dir(copied_path);
  return 0;
}

int test_plaintext_buffer_independent_of_key_file() {
  EncryptionFixture fixture;
  ASSERT_EQ(fixture.error, VXCORE_OK);
  const auto context = fixture.context.value;
  const auto notebook_id = fixture.notebook_id.value;
  EncryptionTestString file_id;
  ASSERT_EQ(vxcore_file_create(context, notebook_id, ".", "ordinary.md", &file_id.value), VXCORE_OK);
  const auto note_path = fixture.path + "/ordinary.md";
  const std::string original = "Plaintext remains editable\n";
  write_file(note_path, original);
  const std::string corrupt_key = "not a valid encryption key file";
  write_file(fixture.key_path(), corrupt_key);

  for (bool unreadable : {false, true}) {
    if (unreadable) {
      std::filesystem::remove(utf8_to_fs_path(fixture.key_path()));
      // A directory at the key-file path is unopenable as a regular key file
      // on every supported platform, including privileged POSIX test runners.
      std::filesystem::create_directory(utf8_to_fs_path(fixture.key_path()));
    }
    EncryptionTestString buffer_id;
    ASSERT_EQ(vxcore_buffer_open(context, notebook_id, "ordinary.md", &buffer_id.value), VXCORE_OK);
    const void *bytes = nullptr;
    size_t size = 0;
    ASSERT_EQ(vxcore_buffer_get_content_raw(context, buffer_id.value, &bytes, &size), VXCORE_OK);
    const auto expected = unreadable ? "Saved despite corrupt key\n" : original;
    ASSERT_EQ(std::string(static_cast<const char *>(bytes), size), expected);
    const std::string saved = unreadable ? "Saved despite unreadable key\n" :
                                           "Saved despite corrupt key\n";
    ASSERT_EQ(vxcore_buffer_set_content_raw(context, buffer_id.value, saved.data(), saved.size()),
              VXCORE_OK);
    ASSERT_EQ(vxcore_buffer_save(context, buffer_id.value), VXCORE_OK);
    ASSERT_EQ(read_file_content(note_path), saved);
    ASSERT_EQ(vxcore_encryption_lock_all(context), VXCORE_OK);
    ASSERT_EQ(vxcore_buffer_reload(context, buffer_id.value), VXCORE_OK);
    ASSERT_EQ(vxcore_buffer_get_content_raw(context, buffer_id.value, &bytes, &size), VXCORE_OK);
    ASSERT_EQ(std::string(static_cast<const char *>(bytes), size), saved);
    ASSERT_EQ(vxcore_buffer_close(context, buffer_id.value), VXCORE_OK);
    if (unreadable) ASSERT(std::filesystem::is_directory(utf8_to_fs_path(fixture.key_path())));
    else ASSERT_EQ(read_file_content(fixture.key_path()), corrupt_key);
  }
  return 0;
}

int test_encryption_status_rejects_malformed_candidates() {
  EncryptionFixture fixture;
  ASSERT_EQ(fixture.error, VXCORE_OK);
  const auto context = fixture.context.value;
  const auto notebook_id = fixture.notebook_id.value;
  ASSERT_EQ(initialize_test_encryption(context, notebook_id), VXCORE_OK);
  EncryptionTestString suffix_id;
  ASSERT_EQ(vxcore_file_create(context, notebook_id, ".", "broken.md.vne", &suffix_id.value), VXCORE_OK);
  write_file(fixture.path + "/broken.md.vne", "not encrypted");
  EncryptionTestString status;
  ASSERT_EQ(vxcore_encryption_get_status(context, notebook_id, "broken.md.vne", &status.value),
            VXCORE_ERR_ENCRYPTION_FORMAT);
  ASSERT_NULL(status.value);
  EncryptionTestString marker_id;
  ASSERT_EQ(vxcore_file_create(context, notebook_id, ".", "marker.md", &marker_id.value), VXCORE_OK);
  ASSERT_EQ(vxcore_node_update_metadata(context, notebook_id, "marker.md", "{\"encrypted\":true}"),
            VXCORE_OK);
  ASSERT_EQ(vxcore_encryption_get_status(context, notebook_id, "marker.md", &status.value),
            VXCORE_ERR_ENCRYPTION_FORMAT);
  ASSERT_NULL(status.value);
  for (const auto *path : {"broken.md.vne", "marker.md"}) {
    EncryptionTestString folder;
    ASSERT_EQ(vxcore_node_get_attachments_folder(context, notebook_id, path, &folder.value),
              VXCORE_ERR_ENCRYPTION_FORMAT);
    ASSERT_NULL(folder.value);
    EncryptionTestString attachments;
    ASSERT_EQ(vxcore_node_list_attachments(context, notebook_id, path, &attachments.value),
              VXCORE_ERR_ENCRYPTION_FORMAT);
    ASSERT_NULL(attachments.value);
  }
  return 0;
}

int test_buffer_hidden_encryption_magic_fails_closed() {
  for (const auto &prefix : {std::string("VNOTEE1\0", 8), std::string("VNEKEY1\0", 8)}) {
    EncryptionFixture fixture;
    ASSERT_EQ(fixture.error, VXCORE_OK);
    const auto context = fixture.context.value;
    const auto notebook_id = fixture.notebook_id.value;
    EncryptionTestString file_id;
    ASSERT_EQ(vxcore_file_create(context, notebook_id, ".", "disguised.md", &file_id.value),
              VXCORE_OK);
    const auto path = fixture.path + "/disguised.md";
    const auto ordinary = prefix.substr(0, 7) + "\nThis is ordinary text\n";
    write_file(path, ordinary);
    {
      EncryptionTestString buffer;
      ASSERT_EQ(vxcore_buffer_open(context, notebook_id, "disguised.md", &buffer.value), VXCORE_OK);
      const void *data = nullptr;
      size_t size = 0;
      ASSERT_EQ(vxcore_buffer_get_content_raw(context, buffer.value, &data, &size), VXCORE_OK);
      ASSERT_EQ(std::string(static_cast<const char *>(data), size), ordinary);
      ASSERT_EQ(vxcore_encryption_lock_all(context), VXCORE_OK);
      ASSERT_EQ(vxcore_buffer_close(context, buffer.value), VXCORE_OK);
    }
    const auto ciphertext = prefix + "not a plaintext document";
    write_file(path, ciphertext);
    EncryptionTestString buffer;
    ASSERT_EQ(vxcore_buffer_open(context, notebook_id, "disguised.md", &buffer.value), VXCORE_OK);
    const void *data = nullptr;
    size_t size = 0;
    ASSERT_EQ(vxcore_buffer_get_content_raw(context, buffer.value, &data, &size),
              VXCORE_ERR_ENCRYPTION_FORMAT);
    // Even a caller ignoring the read error cannot overwrite ciphertext as plaintext.
    const std::string replacement = "replacement text";
    vxcore_buffer_set_content_raw(context, buffer.value, replacement.data(), replacement.size());
    ASSERT_EQ(vxcore_buffer_save(context, buffer.value), VXCORE_ERR_ENCRYPTION_FORMAT);
    ASSERT_EQ(vxcore_buffer_write_backup(context, buffer.value), VXCORE_ERR_ENCRYPTION_FORMAT);
    ASSERT_EQ(read_file_content(path), ciphertext);
    ASSERT_FALSE(path_exists(path + ".vswp"));
    ASSERT_EQ(vxcore_encryption_lock_all(context), VXCORE_ERR_INVALID_STATE);
    ASSERT_EQ(vxcore_buffer_close(context, buffer.value), VXCORE_OK);
    ASSERT_EQ(vxcore_encryption_lock_all(context), VXCORE_OK);
  }
  return 0;
}

std::string encryption_sha256(const std::string &bytes) {
  unsigned char digest[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(digest, reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
  char encoded[crypto_hash_sha256_BYTES * 2 + 1];
  sodium_bin2hex(encoded, sizeof(encoded), digest, sizeof(digest));
  return encoded;
}

int test_encryption_create_note_transaction() {
  EncryptionFixture fixture;
  ASSERT_EQ(fixture.error, VXCORE_OK);
  const auto context = fixture.context.value;
  const auto notebook = fixture.notebook_id.value;
  ASSERT_EQ(initialize_test_encryption(context, notebook), VXCORE_OK);
  const std::string body = "\xEF\xBB\xBF# PRIVATE_CREATED_BODY_7e9d\r\n";
  EncryptionTestString file;
  ASSERT_EQ(vxcore_encryption_create_note(context, notebook, "", "created.md", "markdown",
      body.data(), body.size(), &file.value), VXCORE_OK);
  ASSERT_NOT_NULL(file.value);
  ASSERT_FALSE(path_exists(fixture.path + "/created.md"));
  ASSERT_EQ(read_file_content(fixture.path + "/created.md.vne").find("PRIVATE_CREATED_BODY_7e9d"),
            std::string::npos);
  EncryptionTestString buffer;
  ASSERT_EQ(vxcore_buffer_open_by_node_id(context, file.value, &buffer.value), VXCORE_OK);
  const void *data = nullptr;
  size_t size = 0;
  ASSERT_EQ(vxcore_buffer_get_content_raw(context, buffer.value, &data, &size), VXCORE_OK);
  ASSERT_EQ(std::string(static_cast<const char *>(data), size), body);
  ASSERT_EQ(vxcore_buffer_close(context, buffer.value), VXCORE_OK);
  const auto before = encryption_file_tree(fixture.path);
  EncryptionTestString duplicate;
  ASSERT_EQ(vxcore_encryption_create_note(context, notebook, "", "created.md", "markdown",
      body.data(), body.size(), &duplicate.value), VXCORE_ERR_ALREADY_EXISTS);
  ASSERT_NULL(duplicate.value);
  ASSERT(encryption_file_tree(fixture.path) == before);
  ASSERT_EQ(vxcore_notebook_set_read_only(context, notebook, true), VXCORE_OK);
  ASSERT_EQ(vxcore_encryption_create_note(context, notebook, "", "readonly.txt", "text",
      "", 0, &duplicate.value), VXCORE_ERR_READ_ONLY);
  ASSERT_NULL(duplicate.value);
  ASSERT(encryption_file_tree(fixture.path) == before);
  ASSERT_EQ(vxcore_notebook_set_read_only(context, notebook, false), VXCORE_OK);
  ASSERT_EQ(vxcore_encryption_create_note(context, notebook, "", "empty.txt", "text",
      "", 0, &duplicate.value), VXCORE_OK);
  EncryptionTestString empty;
  ASSERT_EQ(vxcore_buffer_open_by_node_id(context, duplicate.value, &empty.value), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_get_content_raw(context, empty.value, &data, &size), VXCORE_OK);
  ASSERT_EQ(size, static_cast<size_t>(0));
  ASSERT_EQ(vxcore_buffer_close(context, empty.value), VXCORE_OK);
  return 0;
}

int test_key_conflict_blocks_cached_protected_body() {
  EncryptionFixture fixture;
  ASSERT_EQ(fixture.error, VXCORE_OK);
  const auto context = fixture.context.value;
  const auto notebook = fixture.notebook_id.value;
  ASSERT_EQ(initialize_test_encryption(context, notebook), VXCORE_OK);
  EncryptionTestString file, buffer, ordinary;
  const std::string body = "protected cached body";
  ASSERT_EQ(vxcore_encryption_create_note(context, notebook, "", "private.md", "markdown",
      body.data(), body.size(), &file.value), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_open_by_node_id(context, file.value, &buffer.value), VXCORE_OK);
  EncryptionTestGitRepository repository;
  git_repository_init_options options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
  options.flags = GIT_REPOSITORY_INIT_MKPATH;
  options.workdir_path = fixture.path.c_str();
  const auto git_path = fixture.path + "/vx_notebook/vx_sync";
  ASSERT_EQ(git_repository_init_ext(&repository.value, git_path.c_str(), &options), 0);
  const auto key = read_file_content(fixture.key_path());
  git_oid oid;
  ASSERT_EQ(git_blob_create_frombuffer(&oid, repository.value, key.data(), key.size()), 0);
  git_index *raw_index = nullptr;
  ASSERT_EQ(git_repository_index(&raw_index, repository.value), 0);
  std::unique_ptr<git_index, decltype(&git_index_free)> index(raw_index, git_index_free);
  git_index_entry entry{};
  entry.mode = GIT_FILEMODE_BLOB;
  entry.id = oid;
  entry.path = "vx_notebook/encryption.vne";
  ASSERT_EQ(git_index_conflict_add(index.get(), &entry, &entry, &entry), 0);
  ASSERT_EQ(git_index_write(index.get()), 0);
  const void *data = reinterpret_cast<const void *>(1);
  size_t size = 1;
  ASSERT_EQ(vxcore_buffer_get_content_raw(context, buffer.value, &data, &size), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_NULL(data);
  ASSERT_EQ(size, size_t(0));
  ASSERT_EQ(vxcore_buffer_set_content_raw(context, buffer.value, "rejected", 8), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(vxcore_file_create(context, notebook, "", "ordinary.md", &ordinary.value), VXCORE_OK);
  EncryptionTestString plain;
  ASSERT_EQ(vxcore_buffer_open_by_node_id(context, ordinary.value, &plain.value), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_set_content_raw(context, plain.value, "ordinary", 8), VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_save(context, plain.value), VXCORE_OK);
  ASSERT_EQ(read_file_content(fixture.path + "/ordinary.md"), "ordinary");
  ASSERT_EQ(git_index_conflict_cleanup(index.get()), 0);
  ASSERT_EQ(git_index_write(index.get()), 0);
  ASSERT_EQ(vxcore_buffer_get_content_raw(context, buffer.value, &data, &size), VXCORE_OK);
  ASSERT_EQ(std::string(static_cast<const char *>(data), size), body);
  return 0;
}

int test_encryption_protect_complete_resource_closure() {
  EncryptionFixture fixture;
  ASSERT_EQ(fixture.error, VXCORE_OK);
  const auto context = fixture.context.value;
  const auto notebook = fixture.notebook_id.value;
  ASSERT_EQ(initialize_test_encryption(context, notebook), VXCORE_OK);
  EncryptionTestString file;
  ASSERT_EQ(vxcore_file_create(context, notebook, "", "source.md", &file.value), VXCORE_OK);
  const std::string body = "# PRIVATE_CONVERTED_BODY_7e9d\n\nOriginal text\n";
  write_file(fixture.path + "/source.md", body);
  EncryptionTestString buffer;
  ASSERT_EQ(vxcore_buffer_open(context, notebook, "source.md", &buffer.value), VXCORE_OK);
  const auto imported_source = fs_path_to_utf8(fixture.directory.path() / "salary-private.txt");
  const std::string attachment = "PRIVATE_CONVERTED_ATTACHMENT_7e9d";
  write_file(imported_source, attachment);
  EncryptionTestString attachment_name;
  ASSERT_EQ(vxcore_buffer_insert_attachment(context, buffer.value, imported_source.c_str(),
      &attachment_name.value), VXCORE_OK);
  EncryptionTestString assets;
  ASSERT_EQ(vxcore_node_get_attachments_folder(context, notebook, "source.md", &assets.value),
            VXCORE_OK);
  const std::string owned = std::string(assets.value) + "/salary-private.txt";
  const std::string unlisted = std::string(assets.value) + "/unlisted-private.bin";
  const std::string comments = std::string(assets.value) + "/comments.json";
  const std::string unlisted_body = "PRIVATE_UNLISTED_ASSET_7e9d";
  const std::string comment_body = "{\"version\":1,\"comments\":[],\"private\":\"PRIVATE_COMMENT_7e9d\"}";
  write_file(unlisted, unlisted_body);
  write_file(comments, comment_body);
  const auto shared = fs_path_to_utf8(fixture.directory.path() / "shared-original.txt");
  write_file(shared, "SHARED_ORIGINAL_RETAINED_7e9d");
  const std::string dirty = body + "Unsaved current editor paragraph\n";
  ASSERT_EQ(vxcore_buffer_set_content_raw(context, buffer.value, dirty.data(), dirty.size()),
            VXCORE_OK);
  ASSERT_EQ(vxcore_buffer_write_backup(context, buffer.value), VXCORE_OK);
  const char *ids[] = {"11111111-1111-4111-8111-111111111111",
                       "22222222-2222-4222-8222-222222222222",
                       "33333333-3333-4333-8333-333333333333",
                       "44444444-4444-4444-8444-444444444444"};
  nlohmann::json resources = nlohmann::json::array();
  auto add_resource = [&](const char *id, const std::string &path, const char *name,
                          const char *role, bool retain) {
    std::string native_path = path;
#ifdef _WIN32
    std::replace(native_path.begin(), native_path.end(), '/', '\\');
#endif
    resources.push_back({{"resourceId", id}, {"sourcePath", native_path},
        {"sourceSha256", encryption_sha256(read_file_content(path))}, {"name", name},
        {"mediaType", std::string(role) == "comments" ? "application/json" : "application/octet-stream"},
        {"role", role}, {"retainOriginal", retain}});
  };
  add_resource(ids[0], owned, "salary-private.txt", "attachment", false);
  add_resource(ids[1], unlisted, "unlisted-private.bin", "attachment", false);
  add_resource(ids[2], comments, "comments.json", "comments", false);
  add_resource(ids[3], shared, "shared-original.txt", "attachment", true);
  nlohmann::json plan = {{"editorType", "markdown"},
                          {"sourceSha256", encryption_sha256(read_file_content(fixture.path + "/source.md"))},
                          {"resources", resources}};
  const std::string rewritten = dirty + "[attachment](vxasset:" + ids[0] + ")\n";
  const auto before = encryption_file_tree(fixture.path);
  auto stale = plan;
  stale["sourceSha256"] = std::string(64, '0');
  EncryptionTestString output;
  ASSERT_NE(vxcore_encryption_protect_note(context, notebook, "source.md", rewritten.data(),
      rewritten.size(), stale.dump().c_str(), &output.value), VXCORE_OK);
  ASSERT_NULL(output.value);
  ASSERT(encryption_file_tree(fixture.path) == before);
  auto incomplete = plan;
  incomplete["resources"].erase(1);
  ASSERT_NE(vxcore_encryption_protect_note(context, notebook, "source.md", rewritten.data(),
      rewritten.size(), incomplete.dump().c_str(), &output.value), VXCORE_OK);
  ASSERT_NULL(output.value);
  ASSERT(encryption_file_tree(fixture.path) == before);
  ASSERT_EQ(vxcore_encryption_protect_note(context, notebook, "source.md", rewritten.data(),
      rewritten.size(), plan.dump().c_str(), &output.value), VXCORE_OK);
  ASSERT_EQ(std::string(output.value), "source.md.vne");
  ASSERT_FALSE(path_exists(fixture.path + "/source.md"));
  ASSERT_FALSE(path_exists(fixture.path + "/source.md.vswp"));
  ASSERT_FALSE(path_exists(owned));
  ASSERT_FALSE(path_exists(unlisted));
  ASSERT_FALSE(path_exists(comments));
  ASSERT_EQ(read_file_content(shared), "SHARED_ORIGINAL_RETAINED_7e9d");
  ASSERT_EQ(vxcore_buffer_close(context, buffer.value), VXCORE_OK);
  EncryptionTestString reopened;
  ASSERT_EQ(vxcore_buffer_open_by_node_id(context, file.value, &reopened.value), VXCORE_OK);
  const void *data = nullptr;
  size_t size = 0;
  ASSERT_EQ(vxcore_buffer_get_content_raw(context, reopened.value, &data, &size), VXCORE_OK);
  ASSERT_EQ(std::string(static_cast<const char *>(data), size), rewritten);
  EncryptionTestString listed;
  ASSERT_EQ(vxcore_buffer_list_resources(context, reopened.value, &listed.value), VXCORE_OK);
  ASSERT_EQ(nlohmann::json::parse(listed.value).size(), static_cast<size_t>(4));
  ASSERT_EQ(vxcore_buffer_close(context, reopened.value), VXCORE_OK);
  for (const auto &entry : encryption_file_tree(fixture.path)) {
    for (const auto *sentinel : {"PRIVATE_CONVERTED_BODY_7e9d", "PRIVATE_CONVERTED_ATTACHMENT_7e9d",
                                "PRIVATE_UNLISTED_ASSET_7e9d", "PRIVATE_COMMENT_7e9d",
                                "salary-private.txt", "unlisted-private.bin"}) {
      ASSERT_EQ(entry.first.find(sentinel), std::string::npos);
      ASSERT_EQ(entry.second.find(sentinel), std::string::npos);
    }
  }
  const auto profile = fs_path_to_utf8(fixture.directory.path() / "vxcore_test_config");
  for (const auto &entry : encryption_file_tree(profile)) {
    ASSERT_EQ(entry.second.find("salary-private.txt"), std::string::npos);
  }
  return 0;
}

int main() {
  BufferTestTempDirectory test_directory;
  ASSERT_TRUE(test_directory.valid());
  std::cout << "Running buffer tests..." << std::endl;

  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  RUN_TEST(test_buffer_open_close);
  RUN_TEST(test_buffer_get);
  RUN_TEST(test_buffer_list);
  RUN_TEST(test_buffer_content_raw);
  RUN_TEST(test_buffer_content_json);
  RUN_TEST(test_buffer_check_external_changes_unloaded);
  RUN_TEST(test_buffer_mtime_no_jitter);
  RUN_TEST(test_buffer_save_reload);
  RUN_TEST(test_buffer_deduplication);
  RUN_TEST(test_buffer_external_file);
  RUN_TEST(test_buffer_state);
  RUN_TEST(test_buffer_check_external_changes);
  RUN_TEST(test_buffer_external_change_content_aware);
  RUN_TEST(test_buffer_is_modified);
  RUN_TEST(test_buffer_get_revision);

  // Buffer Backup Tests
  RUN_TEST(test_buffer_write_backup);
  RUN_TEST(test_buffer_has_backup);
  RUN_TEST(test_buffer_recover_backup);
  RUN_TEST(test_buffer_discard_backup);
  RUN_TEST(test_buffer_get_backup_path);
  RUN_TEST(test_buffer_backup_path_non_ascii);
  RUN_TEST(test_buffer_backup_no_content);
  RUN_TEST(test_buffer_recover_no_backup);
  RUN_TEST(test_buffer_discard_no_backup);
  RUN_TEST(test_buffer_backup_format);
  RUN_TEST(test_buffer_backup_overwrite);
  RUN_TEST(test_buffer_backup_cleanup_on_save);
  RUN_TEST(test_buffer_backup_cleanup_on_close);

  RUN_TEST(test_buffer_notebook_close);
  RUN_TEST(test_buffer_lazy_loading);
  RUN_TEST(test_buffer_persistence);

  // Buffer Asset Tests (Filesystem Only)
  RUN_TEST(test_buffer_insert_asset_raw);
  RUN_TEST(test_buffer_insert_asset_raw_raw_notebook);
  RUN_TEST(test_buffer_attachments_raw_notebook_unsupported);
  RUN_TEST(test_buffer_asset_surface_raw_notebook);
  RUN_TEST(test_buffer_insert_asset);
  RUN_TEST(test_buffer_delete_asset);
  RUN_TEST(test_buffer_get_assets_folder);

  // Buffer Attachment Tests (Filesystem + Metadata)
  RUN_TEST(test_buffer_insert_attachment);
  RUN_TEST(test_buffer_delete_attachment);
  RUN_TEST(test_buffer_rename_attachment);
  RUN_TEST(test_buffer_legacy_attachment_metadata);
  RUN_TEST(test_buffer_list_attachments);
  RUN_TEST(test_buffer_list_unindexed_attachments);
  RUN_TEST(test_buffer_list_unindexed_attachments_errors);
  RUN_TEST(test_buffer_list_unindexed_attachments_external_unsupported);
  RUN_TEST(test_buffer_get_attachments_folder);

  // External File Asset Tests
  RUN_TEST(test_buffer_external_asset);
  RUN_TEST(test_buffer_asset_unique_name);

  // Buffer Path Update on Rename Tests
  RUN_TEST(test_buffer_rename_file_updates_path);
  RUN_TEST(test_buffer_rename_folder_updates_paths);
  RUN_TEST(test_buffer_rename_no_affect_other_buffers);
  RUN_TEST(test_buffer_rename_provider_functional);

  // Buffer Path Update on Move Tests
  RUN_TEST(test_buffer_move_file_updates_path);
  RUN_TEST(test_buffer_move_folder_updates_paths);
  RUN_TEST(test_buffer_move_no_affect_other_buffers);
  RUN_TEST(test_buffer_move_provider_functional);

  // Buffer Resource Base Path Tests
  RUN_TEST(test_buffer_get_resource_base_path);
  RUN_TEST(test_buffer_get_resource_base_path_subfolder);
  RUN_TEST(test_buffer_get_resource_base_path_external);

  // Buffer Auto-Resolve Tests
  RUN_TEST(test_buffer_open_auto_resolve_notebook);
  RUN_TEST(test_buffer_open_auto_resolve_dedup);
  RUN_TEST(test_buffer_open_auto_resolve_not_in_notebook);
  RUN_TEST(test_buffer_open_auto_resolve_unindexed_file);
  RUN_TEST(test_buffer_open_by_node_id);

  // Virtual Buffer Tests
  RUN_TEST(test_virtual_buffer_open);
  RUN_TEST(test_virtual_buffer_save_noop);
  RUN_TEST(test_virtual_buffer_content_empty);
  RUN_TEST(test_virtual_buffer_state_normal);
  RUN_TEST(test_virtual_buffer_close);
  RUN_TEST(test_virtual_buffer_dedup);
  RUN_TEST(test_virtual_buffer_not_in_session);
  RUN_TEST(test_virtual_buffer_rejected_by_regular_open);

  // Notebook encryption key APIs and the ordinary-buffer independence boundary.
  RUN_TEST(test_encryption_prepare_abandon);
  RUN_TEST(test_encryption_commit_and_existing_key);
  RUN_TEST(test_encryption_setup_context_ownership);
  RUN_TEST(test_encryption_read_only_transitions);
  RUN_TEST(test_encryption_failed_unlock_preserves_state);
  RUN_TEST(test_encryption_rejects_malformed_key_files);
  RUN_TEST(test_encryption_notebook_copy_portability);
  RUN_TEST(test_plaintext_buffer_independent_of_key_file);
  RUN_TEST(test_buffer_hidden_encryption_magic_fails_closed);
  RUN_TEST(test_encryption_status_rejects_malformed_candidates);
  RUN_TEST(test_encryption_create_note_transaction);
  RUN_TEST(test_key_conflict_blocks_cached_protected_body);
  RUN_TEST(test_encryption_protect_complete_resource_closure);

  std::cout << "All buffer tests passed!" << std::endl;
  return 0;
}
