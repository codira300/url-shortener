#include "domain/ShortCode.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace urlshort;

TEST_CASE("short codes accept alphanumeric values") {
  REQUIRE(ShortCode::isValid("a"));
  REQUIRE(ShortCode::isValid("A"));
  REQUIRE(ShortCode::isValid("0"));
  REQUIRE(ShortCode::isValid("abc123XYZ"));
  REQUIRE(ShortCode::isValid(std::string(ShortCode::kMaxLength, 'a')));
}

TEST_CASE("short codes reject empty, oversized and non-alphanumeric values") {
  REQUIRE_FALSE(ShortCode::isValid(""));
  REQUIRE_FALSE(ShortCode::isValid(std::string(ShortCode::kMaxLength + 1, 'a')));
  REQUIRE_FALSE(ShortCode::isValid("abc-123"));
  REQUIRE_FALSE(ShortCode::isValid("abc_123"));
  REQUIRE_FALSE(ShortCode::isValid("abc 123"));
  REQUIRE_FALSE(ShortCode::isValid("abc/def"));
  REQUIRE_FALSE(ShortCode::isValid("abc.def"));
}

TEST_CASE("reserved short codes are matched case-insensitively") {
  REQUIRE(ShortCode::isReserved("api"));
  REQUIRE(ShortCode::isReserved("API"));
  REQUIRE(ShortCode::isReserved("Health"));
  REQUIRE(ShortCode::isReserved("metrics"));
  REQUIRE(ShortCode::isReserved("favicon.ico"));
  REQUIRE(ShortCode::isReserved("Robots.txt"));
}

TEST_CASE("non-reserved short codes pass the reserved check") {
  REQUIRE_FALSE(ShortCode::isReserved("abc123"));
  REQUIRE_FALSE(ShortCode::isReserved(""));
  REQUIRE_FALSE(ShortCode::isReserved(std::string(ShortCode::kMaxLength + 1, 'a')));
}
