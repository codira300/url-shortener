#include "util/Logger.h"

#include "util/Time.h"

#include <string>

namespace urlshort {
namespace {

const char* levelName(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return "debug";
    case LogLevel::Info:
      return "info";
    case LogLevel::Warn:
      return "warn";
    case LogLevel::Error:
      return "error";
  }
  return "unknown";
}

void appendEscaped(std::string& out, std::string_view value) {
  for (char c : value) {
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out.append("?");
        } else {
          out.push_back(c);
        }
    }
  }
}

}  // namespace

LogLevel parseLogLevel(std::string_view value) {
  if (value == "debug") {
    return LogLevel::Debug;
  }
  if (value == "warn") {
    return LogLevel::Warn;
  }
  if (value == "error") {
    return LogLevel::Error;
  }
  return LogLevel::Info;
}

Logger::Logger(LogLevel level, std::ostream& out) : level_(level), out_(out) {}

bool Logger::enabled(LogLevel level) const noexcept {
  return static_cast<int>(level) >= static_cast<int>(level_);
}

void Logger::log(LogLevel level, std::string_view event,
                 std::initializer_list<std::pair<std::string_view, std::string_view>> fields) {
  if (!enabled(level)) {
    return;
  }

  std::string line;
  line.reserve(160);
  line.append("{\"ts\":\"");
  line.append(formatLogTimestamp());
  line.append("\",\"level\":\"");
  line.append(levelName(level));
  line.append("\",\"event\":\"");
  appendEscaped(line, event);
  line.push_back('"');
  for (const auto& [key, value] : fields) {
    line.append(",\"");
    appendEscaped(line, key);
    line.append("\":\"");
    appendEscaped(line, value);
    line.push_back('"');
  }
  line.append("}\n");

  std::lock_guard<std::mutex> lock(mutex_);
  out_ << line;
  out_.flush();
}

}  // namespace urlshort
