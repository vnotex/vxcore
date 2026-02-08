#include "path_provider.h"

#include <cstdlib>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#elif __APPLE__
#include <mach-o/dyld.h>
#include <pwd.h>
#include <unistd.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace vxcore {

std::filesystem::path PathProvider::GetAppDataPath(const std::string &app_name) {
#ifdef _WIN32
  wchar_t *path = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) {
    std::filesystem::path result = std::filesystem::path(path) / app_name;
    CoTaskMemFree(path);
    return result;
  }
  return {};
#elif __APPLE__
  const char *home = getenv("HOME");
  if (!home) {
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
      home = pw->pw_dir;
    }
  }
  if (home) {
    return std::filesystem::path(home) / "Library" / "Application Support" / app_name;
  }
  return {};
#else
  const char *xdg_data_home = getenv("XDG_DATA_HOME");
  if (xdg_data_home) {
    return std::filesystem::path(xdg_data_home) / app_name;
  }
  const char *home = getenv("HOME");
  if (!home) {
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
      home = pw->pw_dir;
    }
  }
  if (home) {
    return std::filesystem::path(home) / ".local" / "share" / app_name;
  }
  return {};
#endif
}

std::filesystem::path PathProvider::GetLocalDataPath(const std::string &app_name) {
#ifdef _WIN32
  wchar_t *path = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
    std::filesystem::path result = std::filesystem::path(path) / app_name;
    CoTaskMemFree(path);
    return result;
  }
  return {};
#elif __APPLE__
  const char *home = getenv("HOME");
  if (!home) {
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
      home = pw->pw_dir;
    }
  }
  if (home) {
    return std::filesystem::path(home) / "Library" / "Caches" / app_name;
  }
  return {};
#else
  const char *xdg_cache_home = getenv("XDG_CACHE_HOME");
  if (xdg_cache_home) {
    return std::filesystem::path(xdg_cache_home) / app_name;
  }
  const char *home = getenv("HOME");
  if (!home) {
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
      home = pw->pw_dir;
    }
  }
  if (home) {
    return std::filesystem::path(home) / ".cache" / app_name;
  }
  return {};
#endif
}

std::filesystem::path PathProvider::GetExecutionFolderPath() {
  auto file_path = GetExecutionFilePath();
  if (!file_path.empty()) {
    return file_path.parent_path();
  }
  return {};
}

std::filesystem::path PathProvider::GetExecutionFilePath() {
#ifdef _WIN32
  wchar_t path[MAX_PATH];
  DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (len > 0) {
    return std::filesystem::path(path);
  }
  return {};
#elif __APPLE__
  char path[1024];
  uint32_t size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) == 0) {
    return std::filesystem::path(path);
  }
  return {};
#else
  char path[1024];
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len != -1) {
    path[len] = '\0';
    return std::filesystem::path(path);
  }
  return {};
#endif
}

}  // namespace vxcore
