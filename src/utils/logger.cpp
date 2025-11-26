#include "logger.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

namespace vxcore {

Logger &Logger::GetInstance() {
  static Logger instance;
  return instance;
}

Logger::Logger() : level_(LogLevel::kInfo), console_enabled_(true), log_file_(nullptr) {
  const char *env_level = std::getenv("VXCORE_LOG_LEVEL");
  if (env_level) {
    std::string level_str = env_level;
    if (level_str == "TRACE") {
      level_ = LogLevel::kTrace;
    } else if (level_str == "DEBUG") {
      level_ = LogLevel::kDebug;
    } else if (level_str == "INFO") {
      level_ = LogLevel::kInfo;
    } else if (level_str == "WARN") {
      level_ = LogLevel::kWarn;
    } else if (level_str == "ERROR") {
      level_ = LogLevel::kError;
    } else if (level_str == "FATAL") {
      level_ = LogLevel::kFatal;
    } else if (level_str == "OFF") {
      level_ = LogLevel::kOff;
    }
  }

  const char *env_file = std::getenv("VXCORE_LOG_FILE");
  if (env_file) {
    SetLogFile(env_file);
  }
}

Logger::~Logger() {
  if (log_file_) {
    fclose(log_file_);
  }
}

void Logger::SetLevel(LogLevel level) {
  std::lock_guard<std::mutex> lock(mutex_);
  level_ = level;
}

LogLevel Logger::GetLevel() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return level_;
}

void Logger::SetLogFile(const std::string &path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (log_file_) {
    fclose(log_file_);
    log_file_ = nullptr;
  }
  log_file_path_ = path;
  if (!path.empty()) {
    log_file_ = fopen(path.c_str(), "a");
  }
}

void Logger::EnableConsole(bool enable) {
  std::lock_guard<std::mutex> lock(mutex_);
  console_enabled_ = enable;
}

void Logger::Log(LogLevel level, const char *file, int line, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  LogImpl(level, file, line, fmt, args);
  va_end(args);
}

void Logger::LogImpl(LogLevel level, const char *file, int line, const char *fmt, va_list args) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (level < level_) {
    return;
  }

  std::string timestamp = GetTimestamp();
  const char *level_str = LevelToString(level);

  const char *filename = strrchr(file, '/');
  if (!filename) {
    filename = strrchr(file, '\\');
  }
  filename = filename ? filename + 1 : file;

  char message[4096];
  vsnprintf(message, sizeof(message), fmt, args);

  char log_line[4096];
  snprintf(log_line, sizeof(log_line), "[%s] [%s] [%s:%d] %s\n", timestamp.c_str(), level_str,
           filename, line, message);

  if (console_enabled_) {
    fprintf(stderr, "%s", log_line);
  }

  if (log_file_) {
    fprintf(log_file_, "%s", log_line);
    fflush(log_file_);
  }
}

const char *Logger::LevelToString(LogLevel level) const {
  switch (level) {
    case LogLevel::kTrace:
      return "TRACE";
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kWarn:
      return "WARN";
    case LogLevel::kError:
      return "ERROR";
    case LogLevel::kFatal:
      return "FATAL";
    default:
      return "UNKNOWN";
  }
}

std::string Logger::GetTimestamp() const {
#ifdef _WIN32
  SYSTEMTIME st;
  GetLocalTime(&st);
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear, st.wMonth,
           st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  return buffer;
#else
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  time_t nowtime = tv.tv_sec;
  struct tm *nowtm = localtime(&nowtime);
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03ld", nowtm->tm_year + 1900,
           nowtm->tm_mon + 1, nowtm->tm_mday, nowtm->tm_hour, nowtm->tm_min, nowtm->tm_sec,
           tv.tv_usec / 1000);
  return buffer;
#endif
}

}  // namespace vxcore
