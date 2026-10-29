#include "app/UrlService.h"
#include "cache/RedisGateway.h"
#include "cache/RedisUrlCache.h"
#include "config/Config.h"
#include "db/PgPool.h"
#include "db/PostgresUrlRepository.h"
#include "domain/ShortCodeGenerator.h"
#include "observability/Metrics.h"
#include "ratelimit/RateLimiter.h"
#include "util/Logger.h"
#include "util/Time.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using namespace urlshort;
using namespace std::chrono_literals;

namespace {

struct Backends {
  Backends()
      : config(Config::fromProcessEnvironment()),
        pool(config.pg, metrics, logger),
        redis(config.redis, metrics, logger) {}

  bool postgresAvailable() { return pool.ping(1s); }
  bool redisAvailable() { return redis.ping(); }

  Config config;
  Logger logger{LogLevel::Error};
  Metrics metrics;
  PgPool pool;
  RedisGateway redis;
};

void deleteRows(PgPool& pool, const std::vector<std::string>& codes) {
  auto lease = pool.acquire(1s);
  if (!lease) {
    return;
  }
  for (const auto& code : codes) {
    (*lease)->execParams("DELETE FROM urls WHERE short_code = $1", {code});
  }
}

}  // namespace

TEST_CASE("PostgreSQL repository persists the URL lifecycle") {
  Backends backends;
  if (!backends.postgresAvailable()) {
    SKIP("PostgreSQL is not available (set PG_DSN or start the development database)");
  }

  RandomShortCodeGenerator generator;
  PostgresUrlRepository repository(backends.pool, backends.metrics, backends.logger, 1s);

  const std::string code = generator.generate(12);
  const std::string expired_code = generator.generate(12);
  const std::string missing_code = generator.generate(12);
  const std::string url = "https://example.com/integration/" + code;

  auto created = repository.create(CreateUrlInput{code, url, std::nullopt});
  REQUIRE(created.status == RepoStatus::Ok);
  REQUIRE(created.record.id > 0);
  REQUIRE(created.record.short_code == code);
  REQUIRE(created.record.original_url == url);
  REQUIRE(created.record.created_epoch > 0);
  REQUIRE_FALSE(created.record.expires_epoch.has_value());
  REQUIRE(created.record.is_active);

  auto found = repository.findByCode(code);
  REQUIRE(found.status == RepoStatus::Ok);
  REQUIRE(found.record.id == created.record.id);
  REQUIRE(found.record.original_url == url);

  auto duplicate = repository.create(CreateUrlInput{code, url, std::nullopt});
  REQUIRE(duplicate.status == RepoStatus::Collision);

  const std::int64_t expires = nowEpochSeconds() + 3600;
  auto with_expiry =
      repository.create(CreateUrlInput{expired_code, "https://example.com/expiring", expires});
  REQUIRE(with_expiry.status == RepoStatus::Ok);
  REQUIRE(with_expiry.record.expires_epoch.has_value());
  REQUIRE(*with_expiry.record.expires_epoch == expires);

  auto missing = repository.findByCode(missing_code);
  REQUIRE(missing.status == RepoStatus::NotFound);

  REQUIRE(repository.deactivate(code).status == RepoStatus::Ok);
  auto deactivated = repository.findByCode(code);
  REQUIRE(deactivated.status == RepoStatus::Ok);
  REQUIRE_FALSE(deactivated.record.is_active);

  REQUIRE(repository.deactivate(code).status == RepoStatus::Ok);
  REQUIRE(repository.deactivate(missing_code).status == RepoStatus::NotFound);

  deleteRows(backends.pool, {code, expired_code});
}

TEST_CASE("Redis cache stores hits, negatives and tombstones") {
  Backends backends;
  if (!backends.redisAvailable()) {
    SKIP("Redis is not available (set REDIS_HOST/REDIS_PORT or start the development cache)");
  }

  RandomShortCodeGenerator generator;
  RedisUrlCache cache(backends.redis, backends.config.cache, backends.metrics, backends.logger);

  const std::string code = generator.generate(12);
  const std::string negative_code = generator.generate(12);
  const std::string key = RedisUrlCache::cacheKey(code);
  const std::string negative_key = RedisUrlCache::cacheKey(negative_code);

  REQUIRE(cache.lookup(code).status == CacheStatus::Miss);

  UrlRecord record;
  record.short_code = code;
  record.original_url = "https://example.com/cached";
  record.created_epoch = nowEpochSeconds();
  record.expires_epoch = nowEpochSeconds() + 600;
  record.is_active = true;

  cache.storeCreated(record);

  auto hit = cache.lookup(code);
  REQUIRE(hit.status == CacheStatus::Hit);
  REQUIRE(hit.record.has_value());
  REQUIRE(hit.record->original_url == record.original_url);
  REQUIRE(hit.record->short_code == code);
  REQUIRE(hit.record->expires_epoch.has_value());

  REQUIRE(cache.invalidate(code));
  REQUIRE(cache.lookup(code).status == CacheStatus::Negative);

  // A stale database read or create-side fill must not overwrite the tombstone.
  cache.populate(record);
  REQUIRE(cache.lookup(code).status == CacheStatus::Negative);
  cache.storeCreated(record);
  REQUIRE(cache.lookup(code).status == CacheStatus::Negative);

  cache.storeNegative(negative_code);
  REQUIRE(cache.lookup(negative_code).status == CacheStatus::Negative);

  backends.redis.del(key);
  backends.redis.del(negative_key);
}

TEST_CASE("Redis rate limiter rejects requests over capacity") {
  Backends backends;
  if (!backends.redisAvailable()) {
    SKIP("Redis is not available (set REDIS_HOST/REDIS_PORT or start the development cache)");
  }

  RandomShortCodeGenerator generator;
  RedisRateLimiter limiter(backends.redis, backends.metrics, backends.logger);
  const std::string identity = "integration:" + generator.generate(12);
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const std::string key =
      "rl:v2:redirect:" + identity + ":" + std::to_string(now_ms / 2000);

  REQUIRE(limiter.check("redirect", identity, 2, 1, false).status == RateLimitStatus::Allowed);
  REQUIRE(limiter.check("redirect", identity, 2, 1, false).status == RateLimitStatus::Allowed);

  const auto pttl = backends.redis.evalInt("return redis.call('PTTL', KEYS[1])", {key}, {});
  REQUIRE(pttl.status == RedisStatus::Ok);
  REQUIRE(pttl.integer.has_value());
  REQUIRE(*pttl.integer > 0);

  const auto limited = limiter.check("redirect", identity, 2, 1, false);
  REQUIRE(limited.status == RateLimitStatus::Limited);
  REQUIRE(limited.retry_after_ms >= 1);

  backends.redis.del(key);
}

TEST_CASE("URL service creates, resolves, exposes metadata and deletes") {
  Backends backends;
  if (!backends.postgresAvailable()) {
    SKIP("PostgreSQL is not available (set PG_DSN or start the development database)");
  }
  if (!backends.redisAvailable()) {
    SKIP("Redis is not available (set REDIS_HOST/REDIS_PORT or start the development cache)");
  }

  RandomShortCodeGenerator generator;
  PostgresUrlRepository repository(backends.pool, backends.metrics, backends.logger, 1s);
  RedisUrlCache cache(backends.redis, backends.config.cache, backends.metrics, backends.logger);
  UrlService service(repository, cache, generator, backends.metrics, backends.logger, 10, 5);

  const std::string url = "https://example.com/service/" + generator.generate(12);
  auto created = service.create(url, std::nullopt);
  REQUIRE(created.status == CreateOutcome::Status::Created);
  REQUIRE(created.record.short_code.size() == 10);
  REQUIRE(created.record.original_url == url);

  auto resolved = service.resolve(created.record.short_code);
  REQUIRE(resolved.status == ResolveOutcome::Status::Found);
  REQUIRE(resolved.record.original_url == url);

  auto metadata = service.metadata(created.record.short_code);
  REQUIRE(metadata.status == MetadataOutcome::Status::Found);
  REQUIRE(metadata.record.short_code == created.record.short_code);

  REQUIRE(service.remove(created.record.short_code).status == DeleteOutcome::Status::Deleted);
  REQUIRE(service.resolve(created.record.short_code).status == ResolveOutcome::Status::NotFound);
  REQUIRE(service.metadata(created.record.short_code).status == MetadataOutcome::Status::Gone);

  REQUIRE(service.create("ftp://example.com", std::nullopt).status ==
          CreateOutcome::Status::ValidationError);
  REQUIRE(service.create(url, nowEpochSeconds() - 60).status == CreateOutcome::Status::Expired);

  deleteRows(backends.pool, {created.record.short_code});
  backends.redis.del(RedisUrlCache::cacheKey(created.record.short_code));
}
