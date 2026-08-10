#ifndef VXCORE_TEST_REPARSE_UTILS_H
#define VXCORE_TEST_REPARSE_UTILS_H

// Helpers for creating reparse points in tests.
//
// A REAL directory junction is mandatory for the symlink/junction containment
// tests: std::filesystem::create_directory_symlink produces
// IO_REPARSE_TAG_SYMLINK, which fs::is_symlink() already reports, so a
// symlink-only fixture cannot detect the junction defect (MSVC maps
// IO_REPARSE_TAG_MOUNT_POINT to the implementation-defined file_type::junction,
// which is_symlink() does NOT report).
//
// Junction creation needs no special privilege, so junction-based tests must
// never be skipped. Symlink creation needs SeCreateSymbolicLinkPrivilege or
// Developer Mode, so symlink-based cases may skip.

#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "test_utils.h"

namespace vxcore_test {

#ifdef _WIN32

// Minimal REPARSE_DATA_BUFFER (normally from ntifs.h, which is not in the SDK
// headers available to user-mode builds).
#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003
#endif

typedef struct _VX_REPARSE_DATA_BUFFER {
  DWORD ReparseTag;
  WORD ReparseDataLength;
  WORD Reserved;
  WORD SubstituteNameOffset;
  WORD SubstituteNameLength;
  WORD PrintNameOffset;
  WORD PrintNameLength;
  WCHAR PathBuffer[1];
} VX_REPARSE_DATA_BUFFER;

// Creates a directory junction at |link_utf8| pointing at |target_utf8|.
// Returns true on success.
inline bool create_junction(const std::string &target_utf8, const std::string &link_utf8) {
  std::error_code ec;
  const std::filesystem::path target_abs =
      std::filesystem::absolute(utf8_to_fs_path(target_utf8), ec);
  if (ec) {
    return false;
  }

  const std::filesystem::path link = utf8_to_fs_path(link_utf8);

  // Build and validate the complete reparse buffer BEFORE opening the handle,
  // so no allocating operation can throw while a handle is held.
  // SubstituteName must be the NT-namespace form; PrintName is the display form.
  const std::wstring substitute = L"\\??\\" + target_abs.wstring();
  const std::wstring print = target_abs.wstring();

  const size_t substitute_bytes = substitute.size() * sizeof(WCHAR);
  const size_t print_bytes = print.size() * sizeof(WCHAR);
  // Both names are NUL-terminated inside the path buffer.
  const size_t path_buffer_bytes = substitute_bytes + sizeof(WCHAR) + print_bytes + sizeof(WCHAR);

  const size_t header_bytes = offsetof(VX_REPARSE_DATA_BUFFER, PathBuffer);
  const size_t total_bytes = header_bytes + path_buffer_bytes;
  if (total_bytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) {
    return false;
  }

  std::vector<char> buffer(total_bytes, 0);
  auto *data = reinterpret_cast<VX_REPARSE_DATA_BUFFER *>(buffer.data());

  data->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
  // ReparseDataLength counts the payload following the 8-byte common header
  // (ReparseTag + ReparseDataLength + Reserved), i.e. the four name fields plus
  // the path buffer.
  data->ReparseDataLength = static_cast<WORD>(
      path_buffer_bytes + (header_bytes - offsetof(VX_REPARSE_DATA_BUFFER, SubstituteNameOffset)));
  data->Reserved = 0;
  data->SubstituteNameOffset = 0;
  data->SubstituteNameLength = static_cast<WORD>(substitute_bytes);
  data->PrintNameOffset = static_cast<WORD>(substitute_bytes + sizeof(WCHAR));
  data->PrintNameLength = static_cast<WORD>(print_bytes);

  memcpy(data->PathBuffer, substitute.c_str(), substitute_bytes + sizeof(WCHAR));
  memcpy(reinterpret_cast<char *>(data->PathBuffer) + data->PrintNameOffset, print.c_str(),
         print_bytes + sizeof(WCHAR));

  const bool created_dir = CreateDirectoryW(link.c_str(), nullptr) != 0;
  if (!created_dir && GetLastError() != ERROR_ALREADY_EXISTS) {
    return false;
  }

  HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (created_dir) {
      RemoveDirectoryW(link.c_str());
    }
    return false;
  }

  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data,
                                  static_cast<DWORD>(total_bytes), nullptr, 0, &returned, nullptr);
  CloseHandle(handle);

  if (!ok) {
    RemoveDirectoryW(link.c_str());
    return false;
  }
  return true;
}

// Removes a junction without following it (RemoveDirectory deletes the link).
inline void remove_reparse_dir(const std::string &link_utf8) {
  RemoveDirectoryW(utf8_to_fs_path(link_utf8).c_str());
}

#else  // !_WIN32

// POSIX has no junctions; a directory symlink is the equivalent reparse point
// and needs no privilege there.
inline bool create_junction(const std::string &target_utf8, const std::string &link_utf8) {
  std::error_code ec;
  std::filesystem::create_directory_symlink(utf8_to_fs_path(target_utf8),
                                            utf8_to_fs_path(link_utf8), ec);
  return !ec;
}

inline void remove_reparse_dir(const std::string &link_utf8) {
  std::error_code ec;
  std::filesystem::remove(utf8_to_fs_path(link_utf8), ec);
}

#endif  // _WIN32

// Best-effort directory symlink. May legitimately fail on Windows without
// SeCreateSymbolicLinkPrivilege / Developer Mode; callers should skip in that
// case rather than fail.
inline bool try_create_directory_symlink(const std::string &target_utf8,
                                         const std::string &link_utf8) {
  std::error_code ec;
  std::filesystem::create_directory_symlink(utf8_to_fs_path(target_utf8),
                                            utf8_to_fs_path(link_utf8), ec);
  return !ec;
}

}  // namespace vxcore_test

#endif  // VXCORE_TEST_REPARSE_UTILS_H
