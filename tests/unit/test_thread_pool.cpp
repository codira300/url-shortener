#include "util/Logger.h"
#include "util/ThreadPool.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace urlshort;
using namespace std::chrono_literals;

TEST_CASE("thread pool rejects work when the bounded queue is full") {
  Logger logger(LogLevel::Error);
  ThreadPool pool(1, 1, logger);
  std::mutex mutex;
  std::condition_variable started_cv;
  bool started = false;
  bool release = false;

  REQUIRE(pool.submit([&] {
    {
      std::unique_lock<std::mutex> lock(mutex);
      started = true;
      started_cv.notify_one();
      started_cv.wait(lock, [&] { return release; });
    }
  }));

  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(started_cv.wait_for(lock, 2s, [&] { return started; }));
  }

  REQUIRE(pool.submit([] {}));
  REQUIRE_FALSE(pool.submit([] {}));

  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  started_cv.notify_all();
  pool.shutdown();
}

TEST_CASE("thread pool discards queued work after the shutdown deadline") {
  Logger logger(LogLevel::Error);
  ThreadPool pool(1, 8, logger);
  std::mutex mutex;
  std::condition_variable started_cv;
  bool started = false;
  bool release = false;
  std::atomic<int> completed{0};

  REQUIRE(pool.submit([&] {
    {
      std::unique_lock<std::mutex> lock(mutex);
      started = true;
      started_cv.notify_one();
      started_cv.wait(lock, [&] { return release; });
    }
    completed.fetch_add(1);
  }));

  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(started_cv.wait_for(lock, 2s, [&] { return started; }));
  }

  REQUIRE(pool.submit([&] { completed.fetch_add(1); }));
  REQUIRE(pool.submit([&] { completed.fetch_add(1); }));

  std::thread releaser([&] {
    std::this_thread::sleep_for(100ms);
    {
      std::lock_guard<std::mutex> lock(mutex);
      release = true;
    }
    started_cv.notify_all();
  });

  const auto shutdown_started = std::chrono::steady_clock::now();
  pool.shutdown(1ms);
  const auto elapsed = std::chrono::steady_clock::now() - shutdown_started;
  releaser.join();

  REQUIRE(elapsed < 2s);
  REQUIRE(completed.load() == 1);
}
