#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace urlshort {

class PgPool;
class RedisGateway;
class Metrics;
class Logger;

// Readiness is deliberately decoupled from the request path. A background
// thread performs at most one PostgreSQL ping and one Redis ping per interval,
// so health probes cannot amplify load during an incident.
//
// Readiness requires both the authoritative database and the shared cache/
// limiter dependency. Redis-only failure is additionally reported as
// "degraded" so operators can distinguish it from a database outage, but a
// replica that cannot consult Redis is removed from rotation by the documented
// health contract.
class HealthService {
 public:
  HealthService(PgPool& pool, RedisGateway& redis, Metrics& metrics, Logger& logger,
                std::chrono::milliseconds interval, std::chrono::milliseconds timeout);
  ~HealthService();

  HealthService(const HealthService&) = delete;
  HealthService& operator=(const HealthService&) = delete;

  void start();
  void stop();

  bool live() const { return true; }
  bool ready() const { return ready_.load(std::memory_order_acquire); }
  bool postgresReady() const { return postgres_ready_.load(std::memory_order_acquire); }
  bool redisReady() const { return redis_ready_.load(std::memory_order_acquire); }

  void refresh();

 private:
  void loop();

  PgPool& pool_;
  RedisGateway& redis_;
  Metrics& metrics_;
  Logger& logger_;
  const std::chrono::milliseconds interval_;
  const std::chrono::milliseconds timeout_;

  std::atomic<bool> started_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> ready_{false};
  std::atomic<bool> postgres_ready_{false};
  std::atomic<bool> redis_ready_{false};

  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread thread_;
};

}  // namespace urlshort
