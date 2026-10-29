#include "util/Time.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

using namespace urlshort;

TEST_CASE("epoch seconds format as RFC 3339 UTC") {
  REQUIRE(formatRfc3339(0) == "1970-01-01T00:00:00Z");
  REQUIRE(formatRfc3339(1700000000) == "2023-11-14T22:13:20Z");
  REQUIRE(formatRfc3339(86400) == "1970-01-02T00:00:00Z");
}

TEST_CASE("RFC 3339 UTC timestamps parse") {
  const auto parsed = parseRfc3339("2023-11-14T22:13:20Z");
  REQUIRE(parsed.has_value());
  REQUIRE(*parsed == 1700000000);
}

TEST_CASE("RFC 3339 offsets are normalized to UTC") {
  REQUIRE(parseRfc3339("2023-11-14T23:13:20+01:00").value_or(-1) == 1700000000);
  REQUIRE(parseRfc3339("2023-11-14T21:13:20-01:00").value_or(-1) == 1700000000);
  REQUIRE(parseRfc3339("2023-11-14T22:13:20+00:00").value_or(-1) == 1700000000);
}

TEST_CASE("fractional seconds are accepted and truncated") {
  REQUIRE(parseRfc3339("2023-11-14T22:13:20.1Z").value_or(-1) == 1700000000);
  REQUIRE(parseRfc3339("2023-11-14T22:13:20.123456789Z").value_or(-1) == 1700000000);
}

TEST_CASE("lowercase separators are accepted") {
  REQUIRE(parseRfc3339("2023-11-14t22:13:20z").value_or(-1) == 1700000000);
}

TEST_CASE("round trips through format and parse") {
  const std::int64_t values[] = {0, 1, 86400, 1700000000, 1900000000};
  for (const auto value : values) {
    CAPTURE(value);
    const auto parsed = parseRfc3339(formatRfc3339(value));
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == value);
  }
}

TEST_CASE("malformed timestamps are rejected") {
  const char* values[] = {
      "",
      "not-a-timestamp",
      "2023-11-14T22:13:20",
      "2023-11-14 22:13:20Z",
      "2023-13-01T00:00:00Z",
      "2023-11-32T00:00:00Z",
      "2023-11-14T24:00:00Z",
      "2023-11-14T22:60:00Z",
      "2023-11-14T22:13:60Z",
      "2023-11-14T22:13:20.Z",
      "2023-11-14T22:13:20+1:00",
      "2023-11-14T22:13:20+25:00",
      "2023-11-14T22:13:20Ztrailing",
      "2023-11-14T22:13:20.1234567890Z",
  };
  for (const char* value : values) {
    CAPTURE(value);
    REQUIRE_FALSE(parseRfc3339(value).has_value());
  }
}

TEST_CASE("log timestamps are millisecond RFC 3339 UTC") {
  const std::string timestamp = formatLogTimestamp();
  REQUIRE(timestamp.size() == 24);
  REQUIRE(timestamp.back() == 'Z');
  REQUIRE(timestamp[4] == '-');
  REQUIRE(timestamp[7] == '-');
  REQUIRE(timestamp[10] == 'T');
  REQUIRE(timestamp[19] == '.');
}
