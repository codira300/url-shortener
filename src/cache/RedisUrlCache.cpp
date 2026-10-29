#include "cache/RedisUrlCache.h"

#include "cache/CacheCodec.h"
#include "cache/RedisGateway.h"
#include "observability/Metrics.h"
#include "util/Logger.h"
#include "util/Time.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace urlshort {
namespace {

constexpr const char* kPopulateScript = R"lua(
local v = redis.call('GET', KEYS[1])
if v and string.sub(v, 1, 1) == 'D' then
  return 0
end
redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2])
return 1
)lua";

constexpr const char* kNegativeScript = R"lua(
local v = redis.call('GET', KEYS[1])
if v then
  local tag = string.sub(v, 1, 1)
  if tag == 'P' or tag == 'D' then
    return 0
  end
end
redis.call('SET', KEYS[1], 'N', 'PX', ARGV[1])
return 1
)lua";

std::mt19937& randomEngine() {
  thread_local std::mt19937 engine(std::random_device{}());
  return engine;
}

}  // namespace

RedisUrlCache::RedisUrlCache(RedisGateway& redis, const CacheConfig& config, Metrics& metrics,
                             Logger& logger)
    : redis_(redis), config_(config), metrics_(metrics), logger_(logger) {}

std::string RedisUrlCache::cacheKey(const std::string& short_code) {
  std::string key;
  key.reserve(12 + short_code.size());
  key.append("url:v1:");
  key.append(short_code);
  return key;
}

int RedisUrlCache::positiveTtlMs(const UrlRecord& record, std::int64_t now) const {
  const std::int64_t now_ms = now * 1000;
  std::int64_t base_ms = static_cast<std::int64_t>(config_.ttl_seconds) * 1000;

  if (record.expires_epoch.has_value()) {
    const std::int64_t remaining_ms = *record.expires_epoch * 1000 - now_ms;
    if (remaining_ms <= 0) {
      return 0;
    }
    base_ms = std::min(base_ms, remaining_ms);
  }
  if (base_ms <= 0) {
    return 0;
  }

  if (config_.ttl_jitter_percent > 0) {
    std::uniform_real_distribution<double> distribution(
        -static_cast<double>(config_.ttl_jitter_percent),
        static_cast<double>(config_.ttl_jitter_percent));
    const double factor = 1.0 + distribution(randomEngine()) / 100.0;
    base_ms = static_cast<std::int64_t>(static_cast<double>(base_ms) * factor);
  }

  if (record.expires_epoch.has_value()) {
    const std::int64_t remaining_ms = *record.expires_epoch * 1000 - now_ms;
    base_ms = std::min(base_ms, remaining_ms);
  }
  if (base_ms < 1) {
    base_ms = 1;
  }
  return static_cast<int>(std::min<std::int64_t>(base_ms, 2'147'483'647));
}

CacheLookupResult RedisUrlCache::lookup(const std::string& short_code) {
  const auto reply = redis_.get(cacheKey(short_code));
  if (reply.status == RedisStatus::Unavailable) {
    metrics_.incCacheUnavailable();
    return CacheLookupResult{CacheStatus::Unavailable, std::nullopt};
  }
  if (reply.status == RedisStatus::NotFound) {
    metrics_.incCacheMiss();
    return CacheLookupResult{CacheStatus::Miss, std::nullopt};
  }

  auto decoded = decodeCacheValue(*reply.value);
  switch (decoded.kind) {
    case CacheValueKind::Positive: {
      if (decoded.record.isExpired(nowEpochSeconds())) {
        metrics_.incCacheNegativeHit();
        return CacheLookupResult{CacheStatus::Negative, std::nullopt};
      }
      decoded.record.short_code = short_code;
      metrics_.incCacheHit();
      return CacheLookupResult{CacheStatus::Hit, std::move(decoded.record)};
    }
    case CacheValueKind::Negative:
    case CacheValueKind::Tombstone:
      metrics_.incCacheNegativeHit();
      return CacheLookupResult{CacheStatus::Negative, std::nullopt};
    case CacheValueKind::Invalid:
      break;
  }

  metrics_.incCacheMiss();
  return CacheLookupResult{CacheStatus::Miss, std::nullopt};
}

void RedisUrlCache::populate(const UrlRecord& record) {
  if (!record.is_active) {
    return;
  }
  const std::int64_t now = nowEpochSeconds();
  const int ttl_ms = positiveTtlMs(record, now);
  if (ttl_ms <= 0) {
    storeNegative(record.short_code);
    return;
  }

  const std::vector<std::string> keys{cacheKey(record.short_code)};
  const std::vector<std::string> args{encodePositiveCacheValue(record), std::to_string(ttl_ms)};
  const auto result = redis_.evalInt(kPopulateScript, keys, args);
  if (result.status == RedisStatus::Unavailable) {
    metrics_.incCacheUnavailable();
    return;
  }
  if (result.integer.has_value() && *result.integer == 0) {
    metrics_.incCacheTombstonePrevented();
  }
}

void RedisUrlCache::storeCreated(const UrlRecord& record) {
  if (!record.is_active) {
    return;
  }
  const int ttl_ms = positiveTtlMs(record, nowEpochSeconds());
  if (ttl_ms <= 0) {
    return;
  }

  // Use the same conditional fill as populate(). A newly inserted code should
  // not have a tombstone, but failing closed is safer than overwriting one if
  // a code is ever reused or an earlier delete raced with this insert.
  const std::vector<std::string> keys{cacheKey(record.short_code)};
  const std::vector<std::string> args{encodePositiveCacheValue(record), std::to_string(ttl_ms)};
  const auto result = redis_.evalInt(kPopulateScript, keys, args);
  if (result.status == RedisStatus::Unavailable) {
    metrics_.incCacheUnavailable();
    return;
  }
  if (result.integer.has_value() && *result.integer == 0) {
    metrics_.incCacheTombstonePrevented();
    logger_.warn("cache_create_fill_prevented_by_tombstone",
                 {{"short_code", record.short_code}});
  }
}

void RedisUrlCache::storeNegative(const std::string& short_code) {
  const std::vector<std::string> keys{cacheKey(short_code)};
  const std::vector<std::string> args{std::to_string(config_.negative_ttl_seconds * 1000)};
  const auto result = redis_.evalInt(kNegativeScript, keys, args);
  if (result.status == RedisStatus::Unavailable) {
    metrics_.incCacheUnavailable();
  }
}

bool RedisUrlCache::invalidate(const std::string& short_code) {
  const auto key = cacheKey(short_code);
  const auto ttl = std::chrono::milliseconds(config_.tombstone_ttl_seconds * 1000);

  constexpr int kAttempts = 3;
  for (int attempt = 1; attempt <= kAttempts; ++attempt) {
    if (redis_.set(key, "D", ttl)) {
      return true;
    }
    if (attempt < kAttempts) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10 * attempt));
    }
  }

  metrics_.incCacheUnavailable();
  logger_.warn("cache_invalidation_failed", {{"short_code", short_code}});
  return false;
}

}  // namespace urlshort
