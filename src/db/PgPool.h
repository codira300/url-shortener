#pragma once

#include "config/Config.h"
#include "db/PgConnection.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace urlshort {

class Logger;
class Metrics;

// Bounded libpq connection pool. Connections are created lazily up to the
// configured maximum and handed out through a move-only Lease. The pool never
// holds the mutex while a connection is in use.
class PgPool {
 public:
  class Lease {
   public:
    Lease() = default;
    Lease(PgPool* pool, std::unique_ptr<PgConnection> connection);
    ~Lease();

    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;

    PgConnection* operator->() const { return connection_.get(); }
    PgConnection& connection() const { return *connection_; }
    void markBroken() { broken_ = true; }
    explicit operator bool() const { return connection_ != nullptr; }

   private:
    void reset();

    PgPool* pool_{nullptr};
    std::unique_ptr<PgConnection> connection_;
    bool broken_{false};
  };

  PgPool(const PgConfig& config, Metrics& metrics, Logger& logger);
  ~PgPool();

  PgPool(const PgPool&) = delete;
  PgPool& operator=(const PgPool&) = delete;

  std::optional<Lease> acquire(std::chrono::milliseconds timeout);
  bool ping(std::chrono::milliseconds timeout);

  std::size_t inUse() const;
  std::size_t idle() const;
  std::size_t total() const;

 private:
  friend class Lease;

  std::unique_ptr<PgConnection> createConnection();
  void release(std::unique_ptr<PgConnection> connection, bool broken);
  void publishMetricsLocked() const;

  PgConfig config_;
  Metrics& metrics_;
  Logger& logger_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::unique_ptr<PgConnection>> idle_;
  std::size_t total_{0};
  std::size_t in_use_{0};
  bool shutting_down_{false};
};

}  // namespace urlshort
