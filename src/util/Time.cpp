#include "util/Time.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace urlshort {
namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool readNumber(std::string_view text, std::size_t pos, std::size_t count, int& value) {
  if (pos + count > text.size()) {
    return false;
  }
  int result = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[pos + i];
    if (!isDigit(c)) {
      return false;
    }
    result = result * 10 + (c - '0');
  }
  value = result;
  return true;
}

}  // namespace

std::int64_t nowEpochSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string formatRfc3339(std::int64_t epoch_seconds) {
  using namespace std::chrono;
  const sys_seconds tp{seconds{epoch_seconds}};
  const sys_days day = floor<days>(tp);
  const year_month_day ymd{day};
  const hh_mm_ss<seconds> time_of_day{tp - day};

  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02u:%02u:%02uZ",
                static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                static_cast<unsigned>(ymd.day()), static_cast<unsigned>(time_of_day.hours().count()),
                static_cast<unsigned>(time_of_day.minutes().count()),
                static_cast<unsigned>(time_of_day.seconds().count()));
  return buffer;
}

std::optional<std::int64_t> parseRfc3339(std::string_view text) {
  using namespace std::chrono;
  if (text.size() < 20) {
    return std::nullopt;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!readNumber(text, 0, 4, year) || text[4] != '-' || !readNumber(text, 5, 2, month) ||
      text[7] != '-' || !readNumber(text, 8, 2, day) || (text[10] != 'T' && text[10] != 't') ||
      !readNumber(text, 11, 2, hour) || text[13] != ':' || !readNumber(text, 14, 2, minute) ||
      text[16] != ':' || !readNumber(text, 17, 2, second)) {
    return std::nullopt;
  }

  std::size_t pos = 19;
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    const std::size_t fraction_start = pos;
    while (pos < text.size() && isDigit(text[pos])) {
      ++pos;
    }
    if (pos == fraction_start || pos - fraction_start > 9) {
      return std::nullopt;
    }
  }

  if (pos >= text.size()) {
    return std::nullopt;
  }

  int offset_seconds = 0;
  const char zone = text[pos];
  if (zone == 'Z' || zone == 'z') {
    ++pos;
  } else if (zone == '+' || zone == '-') {
    int offset_hour = 0;
    int offset_minute = 0;
    if (!readNumber(text, pos + 1, 2, offset_hour) || pos + 3 >= text.size() ||
        text[pos + 3] != ':' || !readNumber(text, pos + 4, 2, offset_minute)) {
      return std::nullopt;
    }
    if (offset_hour > 23 || offset_minute > 59) {
      return std::nullopt;
    }
    offset_seconds = offset_hour * 3600 + offset_minute * 60;
    if (zone == '-') {
      offset_seconds = -offset_seconds;
    }
    pos += 6;
  } else {
    return std::nullopt;
  }

  if (pos != text.size()) {
    return std::nullopt;
  }

  const year_month_day ymd{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
                           std::chrono::day{static_cast<unsigned>(day)}};
  if (!ymd.ok() || hour > 23 || minute > 59 || second > 59) {
    return std::nullopt;
  }

  const sys_days days{ymd};
  const auto seconds_since_epoch =
      std::chrono::seconds{days.time_since_epoch()} + hours{hour} + minutes{minute} + seconds{second} -
      std::chrono::seconds{offset_seconds};
  return seconds_since_epoch.count();
}

std::string formatLogTimestamp() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto seconds_part = floor<seconds>(now);
  const auto millis = duration_cast<milliseconds>(now - seconds_part).count();
  char suffix[32];
  std::snprintf(suffix, sizeof(suffix), ".%03lldZ", static_cast<long long>(millis));
  std::string out = formatRfc3339(seconds_part.time_since_epoch().count());
  out.pop_back();  // drop the trailing 'Z'
  out.append(suffix);
  return out;
}

}  // namespace urlshort
