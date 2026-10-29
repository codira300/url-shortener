#include "app/UrlService.h"
#include "cache/RedisGateway.h"
#include "cache/RedisUrlCache.h"
#include "config/Config.h"
#include "db/PgPool.h"
#include "db/PostgresUrlRepository.h"
#include "domain/ShortCodeGenerator.h"
#include "observability/Metrics.h"
#include "ratelimit/RateLimiter.h"
#include "util/Logger.h"
#include "util/SingleFlight.h"
#include "util/ThreadPool.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace urlshort;
using namespace std::chrono_literals;

namespace {

struct Backends {
  Backends()
      : config(Config::fromProcessEnvironment()),
        pool(config.pg, metrics, logger),
        redis(config.redis, metrics, logger) {}

  bool postgresAvailable() { return pool.ping(1s); }
  bool redisAvailable() { return redis.ping(); }

  Config config;
  Logger logger{LogLevel::Error};
  Metrics metrics;
  PgPool pool;
  RedisGateway redis;
};

void deleteByUrlPrefix(PgPool& pool, const std::string& prefix) {
  auto lease = pool.acquire(1s);
  if (lease) {
    (*lease)->execParams("DELETE FROM urls WHERE original_url LIKE $1", {prefix + "%"});
  }
}

}  // namespace

TEST_CASE("single flight coalesces concurrent loads") {
  SingleFlight<int> flight;
  std::atomic<int> loads{0};
  std::atomic<int> leaders{0};
  std::atomic<int> failures{0};
  constexpr int kThreads = 8;
  std::barrier sync(kThreads);

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      sync.arrive_and_wait();
      auto result = flight.execute(
          "shared-key",
          [&] {
            loads.fetch_add(1);
            std::this_thread::sleep_for(300ms);
            return 42;
          },
          2s);
      if (result.timed_out || !result.value.has_value() || *result.value != 42) {
        failures.fetch_add(1);
      }
      if (result.leader) {
        leaders.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  REQUIRE(failures.load() == 0);
  REQUIRE(loads.load() == 1);
  REQUIRE(leaders.load() == 1);
}

TEST_CASE("thread pool drains every submitted task") {
  Logger logger(LogLevel::Error);
  ThreadPool pool(4, 128, logger);
  std::atomic<int> completed{0};
  constexpr int kTasks = 200;

  int submitted = 0;
  int rejected = 0;
  while (submitted < kTasks) {
    if (pool.submit([&] { completed.fetch_add(1); })) {
      ++submitted;
    } else {
      ++rejected;
      REQUIRE(rejected < 100000);
      std::this_thread::yield();
    }
  }
  pool.shutdown();

  REQUIRE(completed.load() == kTasks);
}

TEST_CASE("concurrent URL lifecycle stays consistent") {
  Backends backends;
  if (!backends.postgresAvailable()) {
    SKIP("PostgreSQL is not available (set PG_DSN or start the development database)");
  }
  if (!backends.redisAvailable()) {
    SKIP("Redis is not available (set REDIS_HOST/REDIS_PORT or start the development cache)");
  }

  RandomShortCodeGenerator generator;
  PostgresUrlRepository repository(backends.pool, backends.metrics, backends.logger, 2s);
  RedisUrlCache cache(backends.redis, backends.config.cache, backends.metrics, backends.logger);
  UrlService service(repository, cache, generator, backends.metrics, backends.logger, 10, 20);

  constexpr int kThreads = 8;
  constexpr int kPerThread = 8;
  const std::string prefix = "https://example.com/concurrency/" + generator.generate(8) + "/";

  std::mutex codes_mutex;
  std::vector<std::string> codes;
  std::atomic<int> failures{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    threads.emplace_back([&, thread_index] {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string url =
            prefix + std::to_string(thread_index) + "-" + std::to_string(i);
        auto outcome = service.create(url, std::nullopt);
        if (outcome.status != CreateOutcome::Status::Created) {
          failures.fetch_add(1);
          continue;
        }
        std::lock_guard<std::mutex> lock(codes_mutex);
        codes.push_back(outcome.record.short_code);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  REQUIRE(failures.load() == 0);
  REQUIRE(codes.size() == static_cast<std::size_t>(kThreads * kPerThread));
  REQUIRE(std::set<std::string>(codes.begin(), codes.end()).size() == codes.size());

  std::atomic<int> resolve_failures{0};
  threads.clear();
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    threads.emplace_back([&, thread_index] {
      for (int i = 0; i < kPerThread; ++i) {
        const auto index = static_cast<std::size_t>(thread_index * kPerThread + i);
        const auto outcome = service.resolve(codes[index]);
        if (outcome.status != ResolveOutcome::Status::Found) {
          resolve_failures.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  REQUIRE(resolve_failures.load() == 0);

  std::atomic<int> delete_failures{0};
  threads.clear();
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    threads.emplace_back([&, thread_index] {
      for (int i = 0; i < kPerThread; ++i) {
        const auto index = static_cast<std::size_t>(thread_index * kPerThread + i);
        if (service.remove(codes[index]).status != DeleteOutcome::Status::Deleted) {
          delete_failures.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  REQUIRE(delete_failures.load() == 0);

  deleteByUrlPrefix(backends.pool, prefix);
  for (const auto& code : codes) {
    backends.redis.del(RedisUrlCache::cacheKey(code));
  }
}

TEST_CASE("concurrent rate limiting never exceeds capacity") {
  Backends backends;
  if (!backends.redisAvailable()) {
    SKIP("Redis is not available (set REDIS_HOST/REDIS_PORT or start the development cache)");
  }

  RandomShortCodeGenerator generator;
  RedisRateLimiter limiter(backends.redis, backends.metrics, backends.logger);
  const std::string identity = "concurrency:" + generator.generate(12);
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const std::string key =
      "rl:v2:redirect:" + identity + ":" + std::to_string(now_ms / 5000);

  constexpr int kThreads = 8;
  constexpr int kPerThread = 10;
  std::atomic<int> allowed{0};
  std::atomic<int> rejected{0};
  std::atomic<int> unavailable{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      for (int attempt = 0; attempt < kPerThread; ++attempt) {
        const auto decision = limiter.check("redirect", identity, 5, 1, false);
        switch (decision.status) {
          case RateLimitStatus::Allowed:
            allowed.fetch_add(1);
            break;
          case RateLimitStatus::Limited:
            rejected.fetch_add(1);
            break;
          case RateLimitStatus::Unavailable:
            unavailable.fetch_add(1);
            break;
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  REQUIRE(unavailable.load() == 0);
  REQUIRE(allowed.load() >= 5);
  REQUIRE(allowed.load() <= 20);
  REQUIRE(rejected.load() > 0);
  REQUIRE(allowed.load() + rejected.load() == kThreads * kPerThread);

  backends.redis.del(key);
}
