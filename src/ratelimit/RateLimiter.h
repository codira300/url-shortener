#pragma once

#include <string_view>

namespace urlshort {

class RedisGateway;
class Metrics;
class Logger;

enum class RateLimitStatus { Allowed, Limited, Unavailable };

struct RateLimitDecision {
  RateLimitStatus status{RateLimitStatus::Unavailable};
  int retry_after_ms{0};
};

class RateLimiter {
 public:
  virtual ~RateLimiter() = default;
  virtual RateLimitDecision check(std::string_view scope, std::string_view identity, int capacity,
                                  int refill_per_second, bool fail_open) = 0;
};

// Distributed fixed-window counter. Each window is a separate key incremented
// with one INCR, and the first hit of a window sets the key's expiry. The
// window length is derived from the configured capacity and refill rate.
class RedisRateLimiter final : public RateLimiter {
 public:
  RedisRateLimiter(RedisGateway& redis, Metrics& metrics, Logger& logger);

  RateLimitDecision check(std::string_view scope, std::string_view identity, int capacity,
                          int refill_per_second, bool fail_open) override;

 private:
  RedisGateway& redis_;
  Metrics& metrics_;
  Logger& logger_;
};

}  // namespace urlshort
