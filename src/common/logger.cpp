#include "common/logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>

namespace minidb {

Logger& Logger::Instance() {
  static Logger logger;
  return logger;
}

void Logger::SetOutputFile(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (path.empty()) {
    file_.close();
    return;
  }
  file_.open(path, std::ios::out | std::ios::app);
}

void Logger::Write(LogLevel level, std::string_view module,
                   const std::string& message) {
  if (!Enabled(level)) return;

  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) %
                  1000;

  std::tm tm_buf{};
  localtime_r(&time, &tm_buf);
  char stamp[32];
  std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", tm_buf.tm_hour,
                tm_buf.tm_min, tm_buf.tm_sec, static_cast<int>(ms.count()));

  std::ostringstream line;
  line << "[" << stamp << "][" << LevelName(level) << "][" << module << "] "
       << message << "\n";
  const std::string text = line.str();

  std::lock_guard<std::mutex> lock(mutex_);
  std::fputs(text.c_str(), stderr);
  if (file_.is_open()) {
    file_ << text;
    file_.flush();
  }
}

LogLevel Logger::ParseLevel(std::string_view text) {
  if (text == "debug") return LogLevel::kDebug;
  if (text == "info") return LogLevel::kInfo;
  if (text == "warn" || text == "warning") return LogLevel::kWarn;
  if (text == "error") return LogLevel::kError;
  if (text == "off" || text == "none") return LogLevel::kOff;
  return LogLevel::kInfo;
}

const char* Logger::LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO ";
    case LogLevel::kWarn:
      return "WARN ";
    case LogLevel::kError:
      return "ERROR";
    case LogLevel::kOff:
      return "OFF  ";
  }
  return "?????";
}

LogMessage::~LogMessage() {
  if (Logger::Instance().Enabled(level_)) {
    Logger::Instance().Write(level_, module_, stream_.str());
  }
}

}  // namespace minidb
