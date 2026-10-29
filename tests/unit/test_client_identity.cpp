#include "net/ClientIdentity.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace urlshort;

TEST_CASE("IP addresses are normalized") {
  REQUIRE(ClientIdentity::normalizeIp("127.0.0.1").value_or("") == "127.0.0.1");
  REQUIRE(ClientIdentity::normalizeIp("  127.0.0.1  ").value_or("") == "127.0.0.1");
  REQUIRE(ClientIdentity::normalizeIp("::1").value_or("") == "::1");
  REQUIRE_FALSE(ClientIdentity::normalizeIp("not-an-ip").has_value());
  REQUIRE_FALSE(ClientIdentity::normalizeIp("").has_value());
}

TEST_CASE("disabled trusted proxy configuration trusts nothing") {
  std::string error;
  const auto config = TrustedProxyConfig::parse(false, "not a cidr", error);
  REQUIRE(config.has_value());
  REQUIRE_FALSE(config->enabled());
  REQUIRE_FALSE(config->trusts("127.0.0.1"));
  REQUIRE(error.empty());
}

TEST_CASE("CIDR blocks are parsed and matched") {
  std::string error;
  const auto config =
      TrustedProxyConfig::parse(true, "127.0.0.1/32,10.0.0.0/8,::1/128", error);
  REQUIRE(config.has_value());
  REQUIRE(error.empty());
  REQUIRE(config->enabled());
  REQUIRE(config->trusts("127.0.0.1"));
  REQUIRE(config->trusts("10.1.2.3"));
  REQUIRE(config->trusts("::1"));
  REQUIRE_FALSE(config->trusts("192.168.1.1"));
  REQUIRE_FALSE(config->trusts("11.0.0.1"));
}

TEST_CASE("invalid CIDR configuration is rejected") {
  std::string error;
  REQUIRE_FALSE(TrustedProxyConfig::parse(true, "not-an-address", error).has_value());
  REQUIRE_FALSE(error.empty());

  error.clear();
  REQUIRE_FALSE(TrustedProxyConfig::parse(true, "10.0.0.0/33", error).has_value());
  REQUIRE_FALSE(error.empty());

  error.clear();
  REQUIRE_FALSE(TrustedProxyConfig::parse(true, "10.0.0.0/abc", error).has_value());
  REQUIRE_FALSE(error.empty());
}

TEST_CASE("forwarded headers are ignored for untrusted peers") {
  std::string error;
  const auto config = TrustedProxyConfig::parse(true, "127.0.0.1/32", error);
  REQUIRE(config.has_value());

  REQUIRE(ClientIdentity::resolve("203.0.113.5", "198.51.100.7", *config) == "203.0.113.5");
}

TEST_CASE("forwarded headers are honored for trusted peers") {
  std::string error;
  const auto config = TrustedProxyConfig::parse(true, "127.0.0.1/32", error);
  REQUIRE(config.has_value());

  REQUIRE(ClientIdentity::resolve("127.0.0.1", "198.51.100.7", *config) == "198.51.100.7");
  REQUIRE(ClientIdentity::resolve("127.0.0.1", "198.51.100.7, 203.0.113.9", *config) ==
          "198.51.100.7");
  REQUIRE(ClientIdentity::resolve("127.0.0.1", " 198.51.100.7 ", *config) == "198.51.100.7");
  REQUIRE(ClientIdentity::resolve("127.0.0.1", "garbage, 198.51.100.7", *config) ==
          "198.51.100.7");
  REQUIRE(ClientIdentity::resolve("127.0.0.1", "", *config) == "127.0.0.1");
}

TEST_CASE("peer addresses are normalized in identities") {
  REQUIRE(ClientIdentity::resolve("::1", "", TrustedProxyConfig{}) == "::1");
  REQUIRE(ClientIdentity::resolve(" 127.0.0.1 ", "", TrustedProxyConfig{}) == "127.0.0.1");
}
