// Structured logger (design doc chapter 13). The only allowed global object.
#pragma once

#include <fstream>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

namespace minidb {

enum class LogLevel { kDebug = 0, kInfo, kWarn, kError, kOff };

class Logger {
 public:
  static Logger& Instance();

  void SetLevel(LogLevel level) { level_ = level; }
  LogLevel level() const { return level_; }
  bool Enabled(LogLevel level) const { return level >= level_; }

  void SetOutputFile(const std::string& path);
  void Write(LogLevel level, std::string_view module, const std::string& message);

  static LogLevel ParseLevel(std::string_view text);
  static const char* LevelName(LogLevel level);

 private:
  Logger() = default;

  std::mutex mutex_;
  LogLevel level_ = LogLevel::kInfo;
  std::ofstream file_;
};

// Usage: MINIDB_LOG_DEBUG("BUF") << "value=" << v;
class LogMessage {
 public:
  LogMessage(LogLevel level, const char* module) : level_(level), module_(module) {}
  ~LogMessage();

  std::ostream& Stream() { return stream_; }

 private:
  LogLevel level_;
  const char* module_;
  std::ostringstream stream_;
};

// Turns the streamed expression into void so it fits in a conditional.
struct LogVoidify {
  void operator&(std::ostream&) {}
};

// A disabled level costs one comparison: the message is neither built nor
// formatted (same trick as glog's LOG_IF). Safe inside if/else without braces.
#define MINIDB_LOG(level, module)                         \
  !::minidb::Logger::Instance().Enabled(level)            \
      ? (void)0                                           \
      : ::minidb::LogVoidify() &                          \
            ::minidb::LogMessage(level, module).Stream()

#define MINIDB_LOG_DEBUG(module) MINIDB_LOG(::minidb::LogLevel::kDebug, module)
#define MINIDB_LOG_INFO(module) MINIDB_LOG(::minidb::LogLevel::kInfo, module)
#define MINIDB_LOG_WARN(module) MINIDB_LOG(::minidb::LogLevel::kWarn, module)
#define MINIDB_LOG_ERROR(module) MINIDB_LOG(::minidb::LogLevel::kError, module)

}  // namespace minidb
