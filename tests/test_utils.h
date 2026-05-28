#ifndef VXCORE_TEST_UTILS_H_
#define VXCORE_TEST_UTILS_H_

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// windows.h #defines a pile of A/W macros (DeleteFile -> DeleteFileA, CreateFile
// -> CreateFileA, etc.) that collide with method names used in vxcore classes
// like SqliteMetadataStore::DeleteFile. Undef the common offenders so that test
// translation units transitively including this header are unaffected.
#undef CopyFile
#undef CreateDirectory
#undef CreateFile
#undef DeleteFile
#undef FindFirstFile
#undef FindNextFile
#undef GetFileAttributes
#undef MoveFile
#undef RemoveDirectory
#endif

#define ASSERT(condition)                                                                        \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << "✗ Assertion failed: " << #condition << " at " << __FILE__ << ":" << __LINE__ \
                << std::endl;                                                                    \
      return 1;                                                                                  \
    }                                                                                            \
  } while (0)

#define ASSERT_EQ(a, b)                                                                      \
  do {                                                                                       \
    if ((a) != (b)) {                                                                        \
      std::cerr << "✗ Assertion failed: " << #a << " == " << #b << " at " << __FILE__ << ":" \
                << __LINE__ << " (" << (a) << " != " << (b) << ")" << std::endl;             \
      return 1;                                                                              \
    }                                                                                        \
  } while (0)

#define ASSERT_NE(a, b)                                                                      \
  do {                                                                                       \
    if ((a) == (b)) {                                                                        \
      std::cerr << "✗ Assertion failed: " << #a << " != " << #b << " at " << __FILE__ << ":" \
                << __LINE__ << " (" << (a) << " == " << (b) << ")" << std::endl;             \
      return 1;                                                                              \
    }                                                                                        \
  } while (0)

#define ASSERT_TRUE(condition) ASSERT(condition)
#define ASSERT_FALSE(condition) ASSERT(!(condition))
#define ASSERT_NULL(ptr) ASSERT((ptr) == nullptr)
#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != nullptr)

#define RUN_TEST(test_func)                                                                \
  do {                                                                                     \
    int result = test_func();                                                              \
    if (result != 0) {                                                                     \
      std::cerr << "✗ Test " << #test_func << " failed at " << __FILE__ << ":" << __LINE__ \
                << std::endl;                                                              \
      return result;                                                                       \
    }                                                                                      \
  } while (0)

// UTF-8-safe path conversion for use inside test infrastructure.
//
// On Windows, std::filesystem and std::ofstream interpret std::string paths
// through the active ANSI codepage, NOT UTF-8 — so non-ASCII paths
// (Chinese / Japanese / accented chars) silently fail. The production
// vxcore::PathFromUtf8 / PathToUtf8 helpers in src/utils/file_utils.h are
// not exported from the DLL (no VXCORE_API), so tests living outside the
// DLL boundary cannot link to them. We re-implement the conversion locally,
// using std::filesystem::path's char8_t/u8string round-trip on POSIX and
// MultiByteToWideChar on Windows.
inline std::filesystem::path utf8_to_fs_path(const std::string &utf8_path) {
#ifdef _WIN32
  if (utf8_path.empty()) {
    return std::filesystem::path();
  }
  int wide_size = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, nullptr, 0);
  if (wide_size <= 0) {
    return std::filesystem::path();
  }
  std::wstring wide_str(static_cast<size_t>(wide_size - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, wide_str.data(), wide_size);
  return std::filesystem::path(wide_str);
#else
  return std::filesystem::path(utf8_path);
#endif
}

inline std::string fs_path_to_utf8(const std::filesystem::path &path) {
#ifdef _WIN32
  const std::wstring &wide_str = path.native();
  if (wide_str.empty()) {
    return std::string();
  }
  int utf8_size =
      WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), static_cast<int>(wide_str.size()), nullptr,
                          0, nullptr, nullptr);
  if (utf8_size <= 0) {
    return std::string();
  }
  std::string out(static_cast<size_t>(utf8_size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide_str.c_str(), static_cast<int>(wide_str.size()), out.data(),
                      utf8_size, nullptr, nullptr);
  return out;
#else
  return path.string();
#endif
}

inline bool path_exists(const std::string &path) {
  return std::filesystem::exists(utf8_to_fs_path(path));
}

inline void remove_directory_recursive(const std::string &path) {
  std::error_code ec;
  std::filesystem::remove_all(utf8_to_fs_path(path), ec);
}

inline void cleanup_test_dir(const std::string &path) {
  if (path_exists(path)) {
    remove_directory_recursive(path);
  }
}

inline void write_file(const std::string &path, const std::string &content) {
  std::ofstream file(utf8_to_fs_path(path), std::ios::binary);
  if (file.is_open()) {
    file.write(content.c_str(), static_cast<std::streamsize>(content.length()));
  }
}

inline void create_directory(const std::string &path) {
  std::error_code ec;
  std::filesystem::create_directories(utf8_to_fs_path(path), ec);
}

// Returns a test path under the system temp directory.
// This ensures test data is created in temp folder, not in the repo root.
inline std::string get_test_path(const std::string &name) {
  return fs_path_to_utf8(std::filesystem::temp_directory_path() / "vxcore_test_data" /
                         utf8_to_fs_path(name));
}
inline std::string normalize_path(const std::string &path) {
  std::string result = path;
  std::replace(result.begin(), result.end(), '\\', '/');
  return result;
}


#endif
