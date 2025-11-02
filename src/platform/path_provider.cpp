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

std::filesystem::path PathProvider::getAppDataPath() {
#ifdef _WIN32
  wchar_t *path = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) {
    std::filesystem::path result = std::filesystem::path(path) / kAppName;
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
    return std::filesystem::path(home) / "Library" / "Application Support" / kAppName;
  }
  return {};
#else
  const char *xdg_data_home = getenv("XDG_DATA_HOME");
  if (xdg_data_home) {
    return std::filesystem::path(xdg_data_home) / kAppName;
  }
  const char *home = getenv("HOME");
  if (!home) {
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
      home = pw->pw_dir;
    }
  }
  if (home) {
    return std::filesystem::path(home) / ".local" / "share" / kAppName;
  }
  return {};
#endif
}

std::filesystem::path PathProvider::getLocalDataPath() {
#ifdef _WIN32
  wchar_t *path = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
    std::filesystem::path result = std::filesystem::path(path) / kAppName;
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
    return std::filesystem::path(home) / "Library" / "Caches" / kAppName;
  }
  return {};
#else
  const char *xdg_cache_home = getenv("XDG_CACHE_HOME");
  if (xdg_cache_home) {
    return std::filesystem::path(xdg_cache_home) / kAppName;
  }
  const char *home = getenv("HOME");
  if (!home) {
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
      home = pw->pw_dir;
    }
  }
  if (home) {
    return std::filesystem::path(home) / ".cache" / kAppName;
  }
  return {};
#endif
}

std::filesystem::path PathProvider::getExecutablePath() {
#ifdef _WIN32
  wchar_t path[MAX_PATH];
  DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (len > 0) {
    return std::filesystem::path(path).parent_path();
  }
  return {};
#elif __APPLE__
  char path[1024];
  uint32_t size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) == 0) {
    return std::filesystem::path(path).parent_path();
  }
  return {};
#else
  char path[1024];
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len != -1) {
    path[len] = '\0';
    return std::filesystem::path(path).parent_path();
  }
  return {};
#endif
}

} // namespace vxcore
