#ifndef VXCORE_UTILS_LOGGER_H_
#define VXCORE_UTILS_LOGGER_H_

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

namespace vxcore {

enum class LogLevel {
  kTrace = 0,
  kDebug = 1,
  kInfo = 2,
  kWarn = 3,
  kError = 4,
  kFatal = 5,
  kOff = 6
};

class Logger {
 public:
  static Logger &GetInstance();

  void SetLevel(LogLevel level);
  LogLevel GetLevel() const;

  void SetLogFile(const std::string &path);
  void EnableConsole(bool enable);

  void Log(LogLevel level, const char *file, int line, const char *fmt, ...);

  Logger(const Logger &) = delete;
  Logger &operator=(const Logger &) = delete;

 private:
  Logger();
  ~Logger();

  void LogImpl(LogLevel level, const char *file, int line, const char *fmt, va_list args);
  const char *LevelToString(LogLevel level) const;
  std::string GetTimestamp() const;

  LogLevel level_;
  bool console_enabled_;
  std::string log_file_path_;
  FILE *log_file_;
  mutable std::mutex mutex_;
};

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif

#define VXCORE_LOG_TRACE(fmt, ...)                                                             \
  do {                                                                                         \
    if (::vxcore::Logger::GetInstance().GetLevel() <= ::vxcore::LogLevel::kTrace) {            \
      ::vxcore::Logger::GetInstance().Log(::vxcore::LogLevel::kTrace, __FILE__, __LINE__, fmt, \
                                          ##__VA_ARGS__);                                      \
    }                                                                                          \
  } while (0)

#define VXCORE_LOG_DEBUG(fmt, ...)                                                             \
  do {                                                                                         \
    if (::vxcore::Logger::GetInstance().GetLevel() <= ::vxcore::LogLevel::kDebug) {            \
      ::vxcore::Logger::GetInstance().Log(::vxcore::LogLevel::kDebug, __FILE__, __LINE__, fmt, \
                                          ##__VA_ARGS__);                                      \
    }                                                                                          \
  } while (0)

#define VXCORE_LOG_INFO(fmt, ...)                                                             \
  do {                                                                                        \
    if (::vxcore::Logger::GetInstance().GetLevel() <= ::vxcore::LogLevel::kInfo) {            \
      ::vxcore::Logger::GetInstance().Log(::vxcore::LogLevel::kInfo, __FILE__, __LINE__, fmt, \
                                          ##__VA_ARGS__);                                     \
    }                                                                                         \
  } while (0)

#define VXCORE_LOG_WARN(fmt, ...)                                                             \
  do {                                                                                        \
    if (::vxcore::Logger::GetInstance().GetLevel() <= ::vxcore::LogLevel::kWarn) {            \
      ::vxcore::Logger::GetInstance().Log(::vxcore::LogLevel::kWarn, __FILE__, __LINE__, fmt, \
                                          ##__VA_ARGS__);                                     \
    }                                                                                         \
  } while (0)

#define VXCORE_LOG_ERROR(fmt, ...)                                                             \
  do {                                                                                         \
    if (::vxcore::Logger::GetInstance().GetLevel() <= ::vxcore::LogLevel::kError) {            \
      ::vxcore::Logger::GetInstance().Log(::vxcore::LogLevel::kError, __FILE__, __LINE__, fmt, \
                                          ##__VA_ARGS__);                                      \
    }                                                                                          \
  } while (0)

#define VXCORE_LOG_FATAL(fmt, ...)                                                             \
  do {                                                                                         \
    if (::vxcore::Logger::GetInstance().GetLevel() <= ::vxcore::LogLevel::kFatal) {            \
      ::vxcore::Logger::GetInstance().Log(::vxcore::LogLevel::kFatal, __FILE__, __LINE__, fmt, \
                                          ##__VA_ARGS__);                                      \
    }                                                                                          \
  } while (0)

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

}  // namespace vxcore

#endif
