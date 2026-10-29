#include "observability/HealthService.h"

#include "cache/RedisGateway.h"
#include "db/PgPool.h"
#include "observability/Metrics.h"
#include "observability/ProcessMetrics.h"
#include "util/Logger.h"

#include <string>

namespace urlshort {

HealthService::HealthService(PgPool& pool, RedisGateway& redis, Metrics& metrics, Logger& logger,
                             std::chrono::milliseconds interval, std::chrono::milliseconds timeout)
    : pool_(pool),
      redis_(redis),
      metrics_(metrics),
      logger_(logger),
      interval_(interval),
      timeout_(timeout) {}

HealthService::~HealthService() { stop(); }

void HealthService::start() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) {
    return;
  }

  refresh();
  thread_ = std::thread([this] { loop(); });
}

void HealthService::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.exchange(true)) {
      return;
    }
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  metrics_.setReady(false);
}

void HealthService::loop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_.load(std::memory_order_acquire)) {
    if (cv_.wait_for(lock, interval_, [this] { return stopping_.load(std::memory_order_acquire); })) {
      break;
    }
    lock.unlock();
    refresh();
    lock.lock();
  }
}

void HealthService::refresh() {
  const bool pg_ok = pool_.ping(timeout_);
  const bool redis_ok = redis_.ping();
  sampleProcessMetrics(metrics_);

  const bool previous_pg = postgres_ready_.exchange(pg_ok, std::memory_order_acq_rel);
  const bool previous_redis = redis_ready_.exchange(redis_ok, std::memory_order_acq_rel);
  const bool is_ready = pg_ok && redis_ok;
  ready_.store(is_ready, std::memory_order_release);
  metrics_.setReady(is_ready);

  if (pg_ok != previous_pg) {
    if (pg_ok) {
      logger_.info("postgres_ready");
    } else {
      logger_.error("postgres_not_ready");
    }
  }
  if (redis_ok != previous_redis) {
    if (redis_ok) {
      logger_.info("redis_ready");
    } else {
      logger_.warn("redis_not_ready",
                   {{"impact", "redirects fall back to PostgreSQL; rate limiting may fail open"}});
    }
  }
}

}  // namespace urlshort
