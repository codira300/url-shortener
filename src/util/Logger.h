#pragma once

#include <initializer_list>
#include <iostream>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace urlshort {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

LogLevel parseLogLevel(std::string_view value);

class Logger {
 public:
  explicit Logger(LogLevel level = LogLevel::Info, std::ostream& out = std::cout);

  bool enabled(LogLevel level) const noexcept;
  void log(LogLevel level, std::string_view event,
           std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {});

  void debug(std::string_view event,
             std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
    log(LogLevel::Debug, event, fields);
  }
  void info(std::string_view event,
            std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
    log(LogLevel::Info, event, fields);
  }
  void warn(std::string_view event,
            std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
    log(LogLevel::Warn, event, fields);
  }
  void error(std::string_view event,
             std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {}) {
    log(LogLevel::Error, event, fields);
  }

 private:
  LogLevel level_;
  std::ostream& out_;
  std::mutex mutex_;
};

}  // namespace urlshort
