#include "cache/RedisGateway.h"
#include "config/Config.h"
#include "db/PgPool.h"
#include "observability/Metrics.h"
#include "util/Logger.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

using namespace urlshort;
using namespace std::chrono_literals;

TEST_CASE("Redis failures are converted to bounded unavailable results") {
  RedisConfig config;
  config.host = "127.0.0.1";
  config.port = 65533;
  config.pool_size = 1;
  config.connect_timeout_ms = 50;
  config.socket_timeout_ms = 50;
  config.pool_wait_timeout_ms = 50;
  config.circuit_failure_threshold = 1;
  config.circuit_cooldown_ms = 50;

  Logger logger(LogLevel::Error);
  Metrics metrics;
  RedisGateway gateway(config, metrics, logger);

  const auto started = std::chrono::steady_clock::now();
  const auto reply = gateway.get("url:v1:missing");
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE(reply.status == RedisStatus::Unavailable);
  REQUIRE_FALSE(reply.value.has_value());
  REQUIRE(elapsed < 2s);
  REQUIRE(gateway.circuitOpen());
  REQUIRE_FALSE(gateway.set("key", "value", 1s));
}

TEST_CASE("PostgreSQL connection failures do not block beyond the acquire timeout") {
  PgConfig config;
  config.dsn = "postgresql://postgres:postgres@127.0.0.1:1/postgres";
  config.pool_size = 1;
  config.connect_timeout_seconds = 1;
  config.statement_timeout_ms = 500;

  Logger logger(LogLevel::Error);
  Metrics metrics;
  PgPool pool(config, metrics, logger);

  const auto started = std::chrono::steady_clock::now();
  const auto lease = pool.acquire(100ms);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE_FALSE(lease.has_value());
  REQUIRE(elapsed < 3s);
  REQUIRE_FALSE(pool.ping(100ms));
}
