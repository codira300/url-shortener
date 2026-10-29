#include "config/Config.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>

using namespace urlshort;

namespace {

std::unordered_map<std::string, std::string> environment(
    std::initializer_list<std::pair<const std::string, std::string>> values) {
  return std::unordered_map<std::string, std::string>(values);
}

}  // namespace

TEST_CASE("empty environment yields documented defaults") {
  const auto config = Config::fromEnvironment({});

  REQUIRE(config.http.host == "127.0.0.1");
  REQUIRE(config.http.port == 8080);
  REQUIRE(config.http.io_threads == 4);
  REQUIRE(config.pg.pool_size == 16);
  REQUIRE(config.redis.host == "127.0.0.1");
  REQUIRE(config.redis.port == 6380);
  REQUIRE(config.cache.ttl_seconds == 300);
  REQUIRE(config.rate_limit.redirect_rps == 2000);
  REQUIRE(config.worker.threads == 16);
  REQUIRE(config.log.level == "info");
  REQUIRE(config.public_base_url == "http://localhost:8080");
  REQUIRE(config.short_code_length == 7);
  REQUIRE_FALSE(config.trust_proxy);
}

TEST_CASE("environment overrides are applied") {
  const auto config = Config::fromEnvironment(environment({
      {"HTTP_HOST", "0.0.0.0"},
      {"HTTP_PORT", "9090"},
      {"HTTP_IO_THREADS", "8"},
      {"PG_DSN", "postgresql://urlshortener:secret@db:5432/urlshortener"},
      {"PG_POOL_SIZE", "32"},
      {"REDIS_HOST", "redis"},
      {"REDIS_PORT", "6379"},
      {"CACHE_TTL_SECONDS", "60"},
      {"WORKER_THREADS", "4"},
      {"WORKER_QUEUE_CAPACITY", "128"},
      {"LOG_LEVEL", "debug"},
      {"LOG_ACCESS", "true"},
      {"PUBLIC_BASE_URL", "https://sho.rt"},
      {"SHORT_CODE_LENGTH", "10"},
      {"SHORT_CODE_MAX_RETRIES", "3"},
      {"TRUST_PROXY", "yes"},
      {"TRUSTED_PROXY_CIDRS", "10.0.0.0/8"},
      {"SHUTDOWN_GRACE_MS", "1500"},
  }));

  REQUIRE(config.http.host == "0.0.0.0");
  REQUIRE(config.http.port == 9090);
  REQUIRE(config.http.io_threads == 8);
  REQUIRE(config.pg.pool_size == 32);
  REQUIRE(config.redis.host == "redis");
  REQUIRE(config.redis.port == 6379);
  REQUIRE(config.cache.ttl_seconds == 60);
  REQUIRE(config.worker.threads == 4);
  REQUIRE(config.worker.queue_capacity == 128);
  REQUIRE(config.log.level == "debug");
  REQUIRE(config.log.access);
  REQUIRE(config.public_base_url == "https://sho.rt");
  REQUIRE(config.short_code_length == 10);
  REQUIRE(config.short_code_max_retries == 3);
  REQUIRE(config.trust_proxy);
  REQUIRE(config.trusted_proxy_cidrs == "10.0.0.0/8");
  REQUIRE(config.shutdown_grace_ms == 1500);
}

TEST_CASE("invalid integers are rejected") {
  REQUIRE_THROWS_AS(Config::fromEnvironment(environment({{"HTTP_PORT", "not-a-number"}})),
                    ConfigError);
  REQUIRE_THROWS_AS(Config::fromEnvironment(environment({{"HTTP_PORT", "70000"}})), ConfigError);
  REQUIRE_THROWS_AS(Config::fromEnvironment(environment({{"HTTP_IO_THREADS", "0"}})), ConfigError);
  REQUIRE_THROWS_AS(Config::fromEnvironment(environment({{"SHORT_CODE_LENGTH", "2"}})),
                    ConfigError);
}

TEST_CASE("invalid booleans are rejected") {
  REQUIRE_THROWS_AS(
      Config::fromEnvironment(environment({{"RATE_LIMIT_FAIL_OPEN_REDIRECT", "perhaps"}})),
      ConfigError);
  REQUIRE_THROWS_AS(Config::fromEnvironment(environment({{"TRUST_PROXY", "2"}})), ConfigError);
}

TEST_CASE("invalid public base URLs are rejected") {
  REQUIRE_THROWS_AS(Config::fromEnvironment(environment({{"PUBLIC_BASE_URL", "sho.rt"}})),
                    ConfigError);
  REQUIRE_THROWS_AS(Config::fromEnvironment(environment({{"PUBLIC_BASE_URL", "ftp://sho.rt"}})),
                    ConfigError);
  REQUIRE_THROWS_AS(Config::fromEnvironment(environment({{"PUBLIC_BASE_URL", "https://sho.rt/"}})),
                    ConfigError);
}

TEST_CASE("invalid log levels are rejected") {
  REQUIRE_THROWS_AS(Config::fromEnvironment(environment({{"LOG_LEVEL", "trace"}})), ConfigError);
}

TEST_CASE("tombstone TTL must not be shorter than negative TTL") {
  REQUIRE_THROWS_AS(Config::fromEnvironment(environment({
                        {"CACHE_NEGATIVE_TTL_SECONDS", "60"},
                        {"CACHE_TOMBSTONE_TTL_SECONDS", "30"},
                    })),
                    ConfigError);
}

TEST_CASE("DSNs with credentials are redacted") {
  REQUIRE(redactDsn("postgresql://user:secret@127.0.0.1:5432/db") ==
          "postgresql://user:***@127.0.0.1:5432/db");
  REQUIRE(redactDsn("host=localhost password=secret port=5432") ==
          "host=localhost password=*** port=5432");
  REQUIRE(redactDsn("postgresql://user@127.0.0.1:5432/db") ==
          "postgresql://user@127.0.0.1:5432/db");
  REQUIRE(redactDsn("postgresql://127.0.0.1:5432/db") == "postgresql://127.0.0.1:5432/db");
}
