#include "db/PgPool.h"

#include "observability/Metrics.h"
#include "util/Logger.h"

#include <chrono>
#include <cstdint>
#include <utility>

namespace urlshort {

PgPool::Lease::Lease(PgPool* pool, std::unique_ptr<PgConnection> connection)
    : pool_(pool), connection_(std::move(connection)) {}

PgPool::Lease::~Lease() { reset(); }

PgPool::Lease::Lease(Lease&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)),
      connection_(std::move(other.connection_)),
      broken_(other.broken_) {
  other.broken_ = false;
}

PgPool::Lease& PgPool::Lease::operator=(Lease&& other) noexcept {
  if (this != &other) {
    reset();
    pool_ = std::exchange(other.pool_, nullptr);
    connection_ = std::move(other.connection_);
    broken_ = other.broken_;
    other.broken_ = false;
  }
  return *this;
}

void PgPool::Lease::reset() {
  if (pool_ != nullptr && connection_ != nullptr) {
    pool_->release(std::move(connection_), broken_);
  }
  pool_ = nullptr;
  connection_.reset();
  broken_ = false;
}

PgPool::PgPool(const PgConfig& config, Metrics& metrics, Logger& logger)
    : config_(config), metrics_(metrics), logger_(logger) {}

PgPool::~PgPool() {
  std::deque<std::unique_ptr<PgConnection>> discarded;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_ = true;
    discarded.swap(idle_);
    if (in_use_ > 0) {
      logger_.warn("pg_pool_destroyed_with_leases",
                   {{"in_use", std::to_string(in_use_)}});
    }
  }
  cv_.notify_all();
}

std::optional<PgPool::Lease> PgPool::acquire(std::chrono::milliseconds timeout) {
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + timeout;
  const auto record_wait = [this, started] {
    const auto elapsed = std::chrono::steady_clock::now() - started;
    metrics_.recordDbPoolWait(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
  };
  std::unique_lock<std::mutex> lock(mutex_);

  for (;;) {
    if (shutting_down_) {
      record_wait();
      return std::nullopt;
    }

    while (!idle_.empty()) {
      auto connection = std::move(idle_.front());
      idle_.pop_front();
      if (!connection->isHealthy()) {
        --total_;
        publishMetricsLocked();
        continue;
      }
      ++in_use_;
      publishMetricsLocked();
      record_wait();
      return Lease(this, std::move(connection));
    }

    if (total_ < static_cast<std::size_t>(config_.pool_size)) {
      ++total_;
      ++in_use_;
      publishMetricsLocked();
      lock.unlock();

      auto connection = createConnection();
      if (!connection) {
        lock.lock();
        --total_;
        --in_use_;
        publishMetricsLocked();
        cv_.notify_one();
        record_wait();
        return std::nullopt;
      }
      record_wait();
      return Lease(this, std::move(connection));
    }

    if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
      metrics_.incDbPoolAcquireTimeout();
      record_wait();
      return std::nullopt;
    }
  }
}

bool PgPool::ping(std::chrono::milliseconds timeout) {
  auto lease = acquire(timeout);
  if (!lease) {
    return false;
  }
  const auto started = std::chrono::steady_clock::now();
  auto result = (*lease)->exec("SELECT 1");
  metrics_.recordDbQuery(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started)
          .count()));
  if (!result.ok()) {
    lease->markBroken();
    metrics_.incDbError();
    return false;
  }
  return true;
}

std::size_t PgPool::inUse() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return in_use_;
}

std::size_t PgPool::idle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return idle_.size();
}

std::size_t PgPool::total() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return total_;
}

std::unique_ptr<PgConnection> PgPool::createConnection() {
  const std::string connect_timeout = std::to_string(config_.connect_timeout_seconds);
  const std::string options = "-c statement_timeout=" + std::to_string(config_.statement_timeout_ms) +
                              " -c idle_in_transaction_session_timeout=5000 -c lock_timeout=1000";

  const char* keys[] = {"dbname", "connect_timeout", "application_name", "options", nullptr};
  const char* values[] = {config_.dsn.c_str(), connect_timeout.c_str(), "urlshortener",
                          options.c_str(), nullptr};

  PGconn* raw = PQconnectdbParams(keys, values, 1);
  if (raw == nullptr) {
    logger_.error("pg_connect_failed", {{"reason", "allocation failure"}});
    return nullptr;
  }
  if (PQstatus(raw) != CONNECTION_OK) {
    logger_.error("pg_connect_failed", {{"reason", PQerrorMessage(raw)}});
    PQfinish(raw);
    return nullptr;
  }

  PQsetClientEncoding(raw, "UTF8");
  logger_.debug("pg_connection_created");
  return std::make_unique<PgConnection>(raw);
}

void PgPool::release(std::unique_ptr<PgConnection> connection, bool broken) {
  if (!connection) {
    return;
  }

  if (!broken) {
    connection->rollbackIfNeeded();
    if (!connection->isHealthy()) {
      broken = true;
    }
  }

  std::unique_ptr<PgConnection> discarded;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    --in_use_;
    if (broken || shutting_down_) {
      --total_;
      discarded = std::move(connection);
    } else {
      idle_.push_back(std::move(connection));
    }
    publishMetricsLocked();
  }
  cv_.notify_one();
  discarded.reset();
}

void PgPool::publishMetricsLocked() const {
  metrics_.setDbPool(static_cast<std::int64_t>(in_use_),
                     static_cast<std::int64_t>(idle_.size()));
}

}  // namespace urlshort
