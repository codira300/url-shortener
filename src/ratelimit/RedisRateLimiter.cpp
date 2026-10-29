#include "ratelimit/RateLimiter.h"

#include "cache/RedisGateway.h"
#include "observability/Metrics.h"
#include "util/Logger.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace urlshort {
namespace {

constexpr const char* kKeyPrefix = "rl:v2:";

// INCR and PEXPIRE must be one atomic operation. Doing them as two commands
// leaves a window in which a process crash can create a key without a TTL.
constexpr const char* kIncrementScript = R"lua(
local count = redis.call('INCR', KEYS[1])
if count == 1 then
  redis.call('PEXPIRE', KEYS[1], ARGV[1])
end
return count
)lua";

std::string makeKey(std::string_view scope, std::string_view identity, long long window_id) {
  std::string key;
  key.reserve(8 + scope.size() + identity.size() + 20);
  key.append(kKeyPrefix);
  key.append(scope);
  key.push_back(':');
  key.append(identity);
  key.push_back(':');
  key.append(std::to_string(window_id));
  return key;
}

}  // namespace

RedisRateLimiter::RedisRateLimiter(RedisGateway& redis, Metrics& metrics, Logger& logger)
    : redis_(redis), metrics_(metrics), logger_(logger) {}

RateLimitDecision RedisRateLimiter::check(std::string_view scope, std::string_view identity,
                                          int capacity, int refill_per_second, bool fail_open) {
  const auto capacity64 = static_cast<long long>(std::max(1, capacity));
  const auto refill64 = static_cast<long long>(std::max(1, refill_per_second));
  const auto window_ms =
      std::max<long long>(1, (capacity64 * 1000 + refill64 - 1) / refill64);

  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const auto window_id = now_ms / window_ms;

  const auto key = makeKey(scope, identity, window_id);
  const std::vector<std::string> keys{key};
  const std::vector<std::string> args{std::to_string(window_ms * 2)};
  const auto result = redis_.evalInt(kIncrementScript, keys, args);
  if (result.status == RedisStatus::Unavailable || !result.integer.has_value()) {
    metrics_.incRateLimitUnavailable(std::string(scope).c_str());
    if (fail_open) {
      metrics_.incRateLimitAllowed(std::string(scope).c_str());
      return RateLimitDecision{RateLimitStatus::Allowed, 0};
    }
    return RateLimitDecision{RateLimitStatus::Unavailable, 0};
  }

  const auto count = *result.integer;
  if (count > capacity64) {
    const auto remaining_ms = window_ms - (now_ms % window_ms);
    const int retry_after_ms =
        static_cast<int>(std::min<long long>(std::max<long long>(1, remaining_ms), 60000));
    metrics_.incRateLimitRejected(std::string(scope).c_str());
    return RateLimitDecision{RateLimitStatus::Limited, retry_after_ms};
  }

  metrics_.incRateLimitAllowed(std::string(scope).c_str());
  return RateLimitDecision{RateLimitStatus::Allowed, 0};
}

}  // namespace urlshort
