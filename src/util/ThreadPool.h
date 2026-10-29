#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace urlshort {

class Logger;

// Fixed-size worker pool with a bounded queue. submit() never blocks: when the
// queue is full the caller must apply backpressure (the HTTP layer responds 503).
class ThreadPool {
 public:
  ThreadPool(std::size_t threads, std::size_t queue_capacity, Logger& logger);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  bool submit(std::function<void()> task);

  // Stops accepting work, drains queued tasks for up to timeout, then joins the
  // workers. Tasks still queued at the deadline are discarded; running tasks
  // cannot be cancelled and must be bounded by their own timeouts (database and
  // Redis calls are).
  void shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

  std::size_t queueDepth() const;
  std::size_t activeCount() const;
  std::size_t threadCount() const;

 private:
  void workerLoop();

  const std::size_t capacity_;
  Logger& logger_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable drained_cv_;
  std::deque<std::function<void()>> queue_;
  std::vector<std::thread> workers_;
  bool stopping_{false};
  std::atomic<std::size_t> active_{0};
  std::atomic<std::size_t> depth_{0};
};

}  // namespace urlshort
