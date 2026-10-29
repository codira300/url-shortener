#include "domain/UrlValidator.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace urlshort;

TEST_CASE("valid http and https URLs are accepted") {
  const char* urls[] = {
      "http://example.com",
      "https://example.com",
      "http://example.com/",
      "https://example.com/path/to/resource?query=1#fragment",
      "HTTP://EXAMPLE.COM",
      "https://sub.example.co.uk:8443/path",
      "http://127.0.0.1:8080/health",
      "https://[::1]/path",
      "https://user:password@example.com/path",
  };
  for (const char* url : urls) {
    CAPTURE(url);
    REQUIRE(validateHttpUrl(url).valid);
  }
}

TEST_CASE("invalid URLs are rejected with a reason") {
  const char* urls[] = {
      "",
      "example.com",
      "www.example.com/path",
      "ftp://example.com",
      "file:///etc/passwd",
      "http://",
      "https://",
      "//example.com",
      "mailto:user@example.com",
      "javascript:alert(1)",
  };
  for (const char* url : urls) {
    CAPTURE(url);
    const auto result = validateHttpUrl(url);
    REQUIRE_FALSE(result.valid);
    REQUIRE_FALSE(result.reason.empty());
  }
}

TEST_CASE("URLs containing control characters are rejected") {
  REQUIRE_FALSE(validateHttpUrl("http://example.com/\n").valid);
  REQUIRE_FALSE(validateHttpUrl("http://example.com/\r").valid);
  REQUIRE_FALSE(validateHttpUrl("http://example.com/\t").valid);
  REQUIRE_FALSE(validateHttpUrl(std::string("http://example.com/\0rest", 25)).valid);
}

TEST_CASE("URLs longer than the limit are rejected") {
  const std::string prefix = "https://example.com/";
  const std::string url = prefix + std::string(kMaxUrlLength - prefix.size() + 1, 'a');
  REQUIRE(url.size() > kMaxUrlLength);
  REQUIRE_FALSE(validateHttpUrl(url).valid);
}
