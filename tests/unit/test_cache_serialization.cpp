#include "cache/CacheCodec.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace urlshort;

TEST_CASE("positive cache values round trip") {
  UrlRecord record;
  record.created_epoch = 1700000000;
  record.expires_epoch = 1700003600;
  record.original_url = "https://example.com/path?q=1";

  const std::string encoded = encodePositiveCacheValue(record);
  REQUIRE(encoded == "P|1700000000|1700003600|https://example.com/path?q=1");

  const auto decoded = decodeCacheValue(encoded);
  REQUIRE(decoded.kind == CacheValueKind::Positive);
  REQUIRE(decoded.record.created_epoch == record.created_epoch);
  REQUIRE(decoded.record.expires_epoch.has_value());
  REQUIRE(*decoded.record.expires_epoch == *record.expires_epoch);
  REQUIRE(decoded.record.original_url == record.original_url);
  REQUIRE(decoded.record.is_active);
}

TEST_CASE("links without an expiry round trip") {
  UrlRecord record;
  record.created_epoch = 42;
  record.original_url = "https://example.com";

  const auto decoded = decodeCacheValue(encodePositiveCacheValue(record));
  REQUIRE(decoded.kind == CacheValueKind::Positive);
  REQUIRE(decoded.record.created_epoch == 42);
  REQUIRE_FALSE(decoded.record.expires_epoch.has_value());
  REQUIRE(decoded.record.original_url == "https://example.com");
}

TEST_CASE("URLs may contain the field separator") {
  UrlRecord record;
  record.created_epoch = 1;
  record.expires_epoch = 2;
  record.original_url = "https://example.com/a|b|c?x=1";

  const auto decoded = decodeCacheValue(encodePositiveCacheValue(record));
  REQUIRE(decoded.kind == CacheValueKind::Positive);
  REQUIRE(decoded.record.original_url == record.original_url);
  REQUIRE(decoded.record.expires_epoch.has_value());
  REQUIRE(*decoded.record.expires_epoch == 2);
}

TEST_CASE("negative and tombstone markers decode") {
  REQUIRE(decodeCacheValue("N").kind == CacheValueKind::Negative);
  REQUIRE(decodeCacheValue("D").kind == CacheValueKind::Tombstone);
}

TEST_CASE("malformed cache values are invalid") {
  const char* values[] = {
      "",
      "P",
      "P|",
      "P|1",
      "P|1|",
      "P|1|2",
      "P|1|2|",
      "P|x|2|https://example.com",
      "P|1|y|https://example.com",
      "P|1|-5|https://example.com",
      "P|1|2",
      "https://example.com",
      "X|1|2|https://example.com",
      "p|1|2|https://example.com",
  };
  for (const char* value : values) {
    CAPTURE(value);
    REQUIRE(decodeCacheValue(value).kind == CacheValueKind::Invalid);
  }
}
