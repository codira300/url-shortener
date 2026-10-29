#include "domain/Base62.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>

using namespace urlshort;

TEST_CASE("base62 encodes zero") { REQUIRE(base62::encode(0) == "0"); }

TEST_CASE("base62 encodes known values") {
  REQUIRE(base62::encode(1) == "1");
  REQUIRE(base62::encode(9) == "9");
  REQUIRE(base62::encode(10) == "A");
  REQUIRE(base62::encode(35) == "Z");
  REQUIRE(base62::encode(36) == "a");
  REQUIRE(base62::encode(61) == "z");
  REQUIRE(base62::encode(62) == "10");
  REQUIRE(base62::encode(3843) == "zz");
}

TEST_CASE("base62 decode inverts encode") {
  const std::uint64_t values[] = {0,
                                  1,
                                  61,
                                  62,
                                  123456,
                                  999999999,
                                  std::numeric_limits<std::uint64_t>::max()};
  for (const auto value : values) {
    const auto decoded = base62::decode(base62::encode(value));
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == value);
  }
}

TEST_CASE("base62 decode accepts leading zeros") {
  const auto decoded = base62::decode("0001");
  REQUIRE(decoded.has_value());
  REQUIRE(*decoded == 1);
}

TEST_CASE("base62 decode rejects invalid input") {
  REQUIRE_FALSE(base62::decode("").has_value());
  REQUIRE_FALSE(base62::decode("abc!").has_value());
  REQUIRE_FALSE(base62::decode("hello world").has_value());
  REQUIRE_FALSE(base62::decode("_-").has_value());
}

TEST_CASE("base62 decode rejects overflow") {
  REQUIRE_FALSE(base62::decode("zzzzzzzzzzzz").has_value());
}
