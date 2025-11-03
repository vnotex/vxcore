#ifndef VXCORE_TEST_UTILS_H_
#define VXCORE_TEST_UTILS_H_

#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define ASSERT(condition)                                                                          \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::cerr << "✗ Assertion failed: " << #condition << " at " << __FILE__ << ":"             \
                << __LINE__ << std::endl;                                                          \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_NULL(ptr) ASSERT((ptr) == nullptr)
#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != nullptr)

inline bool path_exists(const std::string &path) {
#ifdef _WIN32
  return PathFileExistsA(path.c_str());
#else
  struct stat st;
  return stat(path.c_str(), &st) == 0;
#endif
}

inline void remove_directory_recursive(const std::string &path) {
#ifdef _WIN32
  std::string search_path = path + "\\*";
  WIN32_FIND_DATAA fd;
  HANDLE hFind = FindFirstFileA(search_path.c_str(), &fd);
  if (hFind != INVALID_HANDLE_VALUE) {
    do {
      if (strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0) {
        std::string full_path = path + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          remove_directory_recursive(full_path);
        } else {
          DeleteFileA(full_path.c_str());
        }
      }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
  }
  RemoveDirectoryA(path.c_str());
#else
  DIR *dir = opendir(path.c_str());
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
      if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
        std::string full_path = path + "/" + entry->d_name;
        struct stat st;
        if (stat(full_path.c_str(), &st) == 0) {
          if (S_ISDIR(st.st_mode)) {
            remove_directory_recursive(full_path);
          } else {
            unlink(full_path.c_str());
          }
        }
      }
    }
    closedir(dir);
  }
  rmdir(path.c_str());
#endif
}

inline void cleanup_test_dir(const std::string &path) {
  if (path_exists(path)) {
    remove_directory_recursive(path);
  }
}

#endif
