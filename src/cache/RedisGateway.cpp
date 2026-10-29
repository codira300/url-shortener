#include "cache/RedisGateway.h"

#include "observability/Metrics.h"
#include "util/Logger.h"

#include <sw/redis++/redis++.h>

#include <chrono>
#include <exception>
#include <string>

namespace urlshort {
namespace {

class RedisCommandTimer {
 public:
  explicit RedisCommandTimer(Metrics& metrics) : metrics_(metrics) {}

  ~RedisCommandTimer() {
    const auto elapsed = std::chrono::steady_clock::now() - started_;
    metrics_.recordRedisCommand(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
  }

 private:
  Metrics& metrics_;
  const std::chrono::steady_clock::time_point started_{std::chrono::steady_clock::now()};
};

}  // namespace

RedisGateway::CircuitBreaker::CircuitBreaker(int failure_threshold,
                                             std::chrono::milliseconds cooldown, Metrics& metrics,
                                             Logger& logger)
    : failure_threshold_(failure_threshold),
      cooldown_(cooldown),
      metrics_(metrics),
      logger_(logger) {}

bool RedisGateway::CircuitBreaker::allow() {
  if (state_.load(std::memory_order_acquire) == State::Closed) {
    return true;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  if (state_ == State::Open) {
    if (now >= open_until_) {
      state_ = State::HalfOpen;
      state_.store(State::HalfOpen, std::memory_order_release);
      probe_in_flight_ = true;
      return true;
    }
    metrics_.incRedisCircuitOpen();
    return false;
  }
  if (state_ == State::HalfOpen) {
    if (!probe_in_flight_) {
      probe_in_flight_ = true;
      return true;
    }
    metrics_.incRedisCircuitOpen();
    return false;
  }
  return true;
}

void RedisGateway::CircuitBreaker::recordSuccess() {
  if (state_.load(std::memory_order_acquire) == State::Closed) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::Closed) {
    state_ = State::Closed;
    state_.store(State::Closed, std::memory_order_release);
    failures_ = 0;
    probe_in_flight_ = false;
    logger_.info("redis_circuit_closed");
  }
}

void RedisGateway::CircuitBreaker::recordFailure() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == State::Closed) {
    ++failures_;
    if (failures_ < failure_threshold_) {
      return;
    }
  }
  const bool was_open = state_ == State::Open;
  state_ = State::Open;
  state_.store(State::Open, std::memory_order_release);
  open_until_ = std::chrono::steady_clock::now() + cooldown_;
  probe_in_flight_ = false;
  if (!was_open) {
    logger_.warn("redis_circuit_open", {{"failures", std::to_string(failures_)}});
  }
}

RedisGateway::RedisGateway(const RedisConfig& config, Metrics& metrics, Logger& logger)
    : config_(config),
      metrics_(metrics),
      logger_(logger),
      breaker_(config.circuit_failure_threshold,
               std::chrono::milliseconds(config.circuit_cooldown_ms), metrics, logger) {}

RedisGateway::~RedisGateway() = default;

sw::redis::Redis* RedisGateway::client() {
  if (auto* existing = client_.load(std::memory_order_acquire); existing != nullptr) {
    return existing;
  }

  std::lock_guard<std::mutex> lock(init_mutex_);
  if (owned_) {
    return owned_.get();
  }

  try {
    sw::redis::ConnectionOptions options;
    options.host = config_.host;
    options.port = config_.port;
    options.db = config_.db;
    if (!config_.password.empty()) {
      options.password = config_.password;
    }
    options.connect_timeout = std::chrono::milliseconds(config_.connect_timeout_ms);
    options.socket_timeout = std::chrono::milliseconds(config_.socket_timeout_ms);
    options.keep_alive = true;

    sw::redis::ConnectionPoolOptions pool;
    pool.size = static_cast<std::size_t>(config_.pool_size);
    pool.wait_timeout = std::chrono::milliseconds(config_.pool_wait_timeout_ms);

    owned_ = std::make_unique<sw::redis::Redis>(options, pool);
    client_.store(owned_.get(), std::memory_order_release);
    logger_.debug("redis_client_initialized");
    return owned_.get();
  } catch (const std::exception& error) {
    logger_.warn("redis_client_init_failed", {{"detail", error.what()}});
    return nullptr;
  }
}

RedisReply RedisGateway::get(std::string_view key) {
  if (!breaker_.allow()) {
    return RedisReply{RedisStatus::Unavailable, std::nullopt};
  }
  auto* redis = client();
  if (redis == nullptr) {
    breaker_.recordFailure();
    return RedisReply{RedisStatus::Unavailable, std::nullopt};
  }
  RedisCommandTimer timer(metrics_);
  try {
    auto value = redis->get(key);
    breaker_.recordSuccess();
    if (!value) {
      return RedisReply{RedisStatus::NotFound, std::nullopt};
    }
    return RedisReply{RedisStatus::Ok, std::string(*value)};
  } catch (const std::exception&) {
    metrics_.incRedisError();
    breaker_.recordFailure();
    return RedisReply{RedisStatus::Unavailable, std::nullopt};
  }
}

bool RedisGateway::set(std::string_view key, std::string_view value,
                       std::chrono::milliseconds ttl) {
  if (!breaker_.allow()) {
    return false;
  }
  auto* redis = client();
  if (redis == nullptr) {
    breaker_.recordFailure();
    return false;
  }
  RedisCommandTimer timer(metrics_);
  try {
    const bool stored = redis->set(key, value, ttl);
    breaker_.recordSuccess();
    return stored;
  } catch (const std::exception&) {
    metrics_.incRedisError();
    breaker_.recordFailure();
    return false;
  }
}

bool RedisGateway::del(std::string_view key) {
  if (!breaker_.allow()) {
    return false;
  }
  auto* redis = client();
  if (redis == nullptr) {
    breaker_.recordFailure();
    return false;
  }
  RedisCommandTimer timer(metrics_);
  try {
    const long long removed = redis->del(key);
    breaker_.recordSuccess();
    return removed > 0;
  } catch (const std::exception&) {
    metrics_.incRedisError();
    breaker_.recordFailure();
    return false;
  }
}

std::optional<long long> RedisGateway::incr(std::string_view key) {
  if (!breaker_.allow()) {
    return std::nullopt;
  }
  auto* redis = client();
  if (redis == nullptr) {
    breaker_.recordFailure();
    return std::nullopt;
  }
  RedisCommandTimer timer(metrics_);
  try {
    const long long value = redis->incr(key);
    breaker_.recordSuccess();
    return value;
  } catch (const std::exception&) {
    metrics_.incRedisError();
    breaker_.recordFailure();
    return std::nullopt;
  }
}

bool RedisGateway::pexpire(std::string_view key, std::chrono::milliseconds ttl) {
  if (!breaker_.allow()) {
    return false;
  }
  auto* redis = client();
  if (redis == nullptr) {
    breaker_.recordFailure();
    return false;
  }
  RedisCommandTimer timer(metrics_);
  try {
    const bool updated = redis->pexpire(key, ttl);
    breaker_.recordSuccess();
    return updated;
  } catch (const std::exception&) {
    metrics_.incRedisError();
    breaker_.recordFailure();
    return false;
  }
}

RedisEvalResult RedisGateway::evalInt(std::string_view script, const std::vector<std::string>& keys,
                                      const std::vector<std::string>& args) {
  if (!breaker_.allow()) {
    return RedisEvalResult{RedisStatus::Unavailable, std::nullopt};
  }
  auto* redis = client();
  if (redis == nullptr) {
    breaker_.recordFailure();
    return RedisEvalResult{RedisStatus::Unavailable, std::nullopt};
  }
  RedisCommandTimer timer(metrics_);
  try {
    const std::string script_key(script);
    std::string sha;
    {
      std::lock_guard<std::mutex> lock(script_mutex_);
      const auto found = script_shas_.find(script_key);
      if (found != script_shas_.end()) {
        sha = found->second;
      }
    }

    long long value = 0;
    bool executed = false;
    bool script_flushed = false;
    if (!sha.empty()) {
      try {
        value = redis->evalsha<long long>(sha, keys.begin(), keys.end(), args.begin(),
                                          args.end());
        executed = true;
      } catch (const sw::redis::Error& error) {
        if (std::string(error.what()).find("NOSCRIPT") == std::string::npos) {
          throw;
        }
        script_flushed = true;
      }
    }

    if (!executed) {
      std::lock_guard<std::mutex> lock(script_mutex_);
      if (script_flushed) {
        sha = redis->script_load(script);
        script_shas_[script_key] = sha;
      } else {
        const auto found = script_shas_.find(script_key);
        if (found != script_shas_.end()) {
          sha = found->second;
        } else {
          sha = redis->script_load(script);
          script_shas_.emplace(script_key, sha);
        }
      }
      value = redis->evalsha<long long>(sha, keys.begin(), keys.end(), args.begin(),
                                        args.end());
    }

    breaker_.recordSuccess();
    return RedisEvalResult{RedisStatus::Ok, value};
  } catch (const std::exception&) {
    metrics_.incRedisError();
    breaker_.recordFailure();
    return RedisEvalResult{RedisStatus::Unavailable, std::nullopt};
  }
}

bool RedisGateway::scriptFlush() {
  if (!breaker_.allow()) {
    return false;
  }
  auto* redis = client();
  if (redis == nullptr) {
    breaker_.recordFailure();
    return false;
  }
  RedisCommandTimer timer(metrics_);
  try {
    redis->script_flush();
    {
      std::lock_guard<std::mutex> lock(script_mutex_);
      script_shas_.clear();
    }
    breaker_.recordSuccess();
    return true;
  } catch (const std::exception&) {
    metrics_.incRedisError();
    breaker_.recordFailure();
    return false;
  }
}

bool RedisGateway::ping() {
  if (!breaker_.allow()) {
    return false;
  }
  auto* redis = client();
  if (redis == nullptr) {
    breaker_.recordFailure();
    return false;
  }
  RedisCommandTimer timer(metrics_);
  try {
    redis->ping();
    breaker_.recordSuccess();
    return true;
  } catch (const std::exception&) {
    metrics_.incRedisError();
    breaker_.recordFailure();
    return false;
  }
}

bool RedisGateway::circuitOpen() const { return breaker_.isOpen(); }

}  // namespace urlshort
