#pragma once

#include "config/Config.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sw::redis {
class Redis;
}

namespace urlshort {

class Logger;
class Metrics;

enum class RedisStatus { Ok, NotFound, Unavailable };

struct RedisReply {
  RedisStatus status{RedisStatus::Unavailable};
  std::optional<std::string> value;
};

struct RedisEvalResult {
  RedisStatus status{RedisStatus::Unavailable};
  std::optional<long long> integer;
};

// Thread-safe Redis facade. redis-plus-plus provides the connection pool; this
// class adds timeouts, a circuit breaker and non-throwing result types so that
// cache and rate-limit outages cannot turn into an unbounded latency problem.
class RedisGateway {
 public:
  RedisGateway(const RedisConfig& config, Metrics& metrics, Logger& logger);
  ~RedisGateway();

  RedisGateway(const RedisGateway&) = delete;
  RedisGateway& operator=(const RedisGateway&) = delete;

  RedisReply get(std::string_view key);
  bool set(std::string_view key, std::string_view value, std::chrono::milliseconds ttl);
  bool del(std::string_view key);
  std::optional<long long> incr(std::string_view key);
  bool pexpire(std::string_view key, std::chrono::milliseconds ttl);
  RedisEvalResult evalInt(std::string_view script, const std::vector<std::string>& keys,
                          const std::vector<std::string>& args);
  bool ping();

  // Administrative/test hook: flushes Redis scripts and the local SHA cache.
  bool scriptFlush();

  bool circuitOpen() const;

 private:
  class CircuitBreaker {
   public:
    CircuitBreaker(int failure_threshold, std::chrono::milliseconds cooldown, Metrics& metrics,
                   Logger& logger);
    bool allow();
    void recordSuccess();
    void recordFailure();
    bool isOpen() const {
      return state_.load(std::memory_order_acquire) != State::Closed;
    }

   private:
    enum class State { Closed, Open, HalfOpen };

    int failure_threshold_;
    std::chrono::milliseconds cooldown_;
    Metrics& metrics_;
    Logger& logger_;
    mutable std::mutex mutex_;
    std::atomic<State> state_{State::Closed};
    int failures_{0};
    bool probe_in_flight_{false};
    std::chrono::steady_clock::time_point open_until_{};
  };

  sw::redis::Redis* client();

  RedisConfig config_;
  Metrics& metrics_;
  Logger& logger_;
  CircuitBreaker breaker_;
  std::mutex init_mutex_;
  std::mutex script_mutex_;
  std::unordered_map<std::string, std::string> script_shas_;
  std::unique_ptr<sw::redis::Redis> owned_;
  std::atomic<sw::redis::Redis*> client_{nullptr};
};

}  // namespace urlshort
