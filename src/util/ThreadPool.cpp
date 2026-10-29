#include "util/ThreadPool.h"

#include "util/Logger.h"

#include <chrono>
#include <exception>
#include <utility>

namespace urlshort {

ThreadPool::ThreadPool(std::size_t threads, std::size_t queue_capacity, Logger& logger)
    : capacity_(queue_capacity), logger_(logger) {
  workers_.reserve(threads);
  for (std::size_t i = 0; i < threads; ++i) {
    workers_.emplace_back([this] { workerLoop(); });
  }
}

ThreadPool::~ThreadPool() { shutdown(); }

bool ThreadPool::submit(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || queue_.size() >= capacity_) {
      return false;
    }
    queue_.push_back(std::move(task));
    depth_.store(queue_.size(), std::memory_order_relaxed);
  }
  cv_.notify_one();
  return true;
}

void ThreadPool::shutdown(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t discarded = 0;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    stopping_ = true;
    cv_.notify_all();

    drained_cv_.wait_until(lock, deadline, [this] {
      return queue_.empty() && active_.load(std::memory_order_relaxed) == 0;
    });

    discarded = queue_.size();
    queue_.clear();
    depth_.store(0, std::memory_order_relaxed);
    cv_.notify_all();
  }

  if (discarded > 0) {
    logger_.warn("thread_pool_shutdown_discarded_tasks",
                 {{"tasks", std::to_string(discarded)}});
  }

  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

std::size_t ThreadPool::queueDepth() const { return depth_.load(std::memory_order_relaxed); }

std::size_t ThreadPool::activeCount() const { return active_.load(std::memory_order_relaxed); }

std::size_t ThreadPool::threadCount() const { return workers_.size(); }

void ThreadPool::workerLoop() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_) {
          return;
        }
        continue;
      }
      task = std::move(queue_.front());
      queue_.pop_front();
      depth_.store(queue_.size(), std::memory_order_relaxed);
      active_.fetch_add(1, std::memory_order_relaxed);
    }

    try {
      task();
    } catch (const std::exception& error) {
      logger_.error("worker_exception", {{"what", error.what()}});
    } catch (...) {
      logger_.error("worker_exception", {{"what", "unknown exception"}});
    }
    active_.fetch_sub(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.empty() && active_.load(std::memory_order_relaxed) == 0) {
        drained_cv_.notify_all();
      }
    }
  }
}

}  // namespace urlshort
