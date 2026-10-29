#include "app/UrlService.h"
#include "cache/RedisUrlCache.h"
#include "db/UrlRepository.h"
#include "domain/ShortCodeGenerator.h"
#include "observability/Metrics.h"
#include "util/Logger.h"
#include "util/Time.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace urlshort;
using namespace std::chrono_literals;

namespace {

class FixedGenerator final : public ShortCodeGenerator {
 public:
  explicit FixedGenerator(std::vector<std::string> codes) : codes_(std::move(codes)) {}

  std::string generate(std::size_t) override {
    if (next_ >= codes_.size()) {
      return "fallback";
    }
    return codes_[next_++];
  }

  int calls() const { return static_cast<int>(next_); }

 private:
  std::vector<std::string> codes_;
  std::size_t next_{0};
};

class FakeRepository final : public UrlRepository {
 public:
  RepoResult create(const CreateUrlInput& input) override {
    ++create_calls;
    if (collisions_remaining > 0) {
      --collisions_remaining;
      return RepoResult{RepoStatus::Collision, {}, "duplicate code"};
    }
    if (next_create_status != RepoStatus::Ok) {
      return RepoResult{next_create_status, {}, "forced create failure"};
    }

    UrlRecord record;
    record.id = next_id++;
    record.short_code = input.short_code;
    record.original_url = input.original_url;
    record.created_epoch = nowEpochSeconds();
    record.expires_epoch = input.expires_epoch;
    record.is_active = true;
    rows[record.short_code] = record;
    return RepoResult{RepoStatus::Ok, std::move(record), {}};
  }

  RepoResult findByCode(const std::string& short_code) override {
    ++find_calls;
    if (next_find_status != RepoStatus::Ok) {
      return RepoResult{next_find_status, {}, "forced find failure"};
    }
    const auto it = rows.find(short_code);
    if (it == rows.end()) {
      return RepoResult{RepoStatus::NotFound, {}, {}};
    }
    if (delay_find > 0ms) {
      std::this_thread::sleep_for(delay_find);
    }
    return RepoResult{RepoStatus::Ok, it->second, {}};
  }

  RepoResult deactivate(const std::string& short_code) override {
    ++deactivate_calls;
    if (next_deactivate_status != RepoStatus::Ok) {
      return RepoResult{next_deactivate_status, {}, "forced deactivate failure"};
    }
    const auto it = rows.find(short_code);
    if (it == rows.end()) {
      return RepoResult{RepoStatus::NotFound, {}, {}};
    }
    it->second.is_active = false;
    return RepoResult{RepoStatus::Ok, {}, {}};
  }

  bool ping() override { return true; }

  std::unordered_map<std::string, UrlRecord> rows;
  RepoStatus next_create_status{RepoStatus::Ok};
  RepoStatus next_find_status{RepoStatus::Ok};
  RepoStatus next_deactivate_status{RepoStatus::Ok};
  int collisions_remaining{0};
  int create_calls{0};
  int find_calls{0};
  int deactivate_calls{0};
  std::int64_t next_id{1};
  std::chrono::milliseconds delay_find{0};
};

class FakeCache final : public UrlCache {
 public:
  CacheLookupResult lookup(const std::string& short_code) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++lookups;
    if (unavailable) {
      return CacheLookupResult{CacheStatus::Unavailable, std::nullopt};
    }
    if (negatives.contains(short_code)) {
      return CacheLookupResult{CacheStatus::Negative, std::nullopt};
    }
    const auto it = positives.find(short_code);
    if (it == positives.end()) {
      return CacheLookupResult{CacheStatus::Miss, std::nullopt};
    }
    return CacheLookupResult{CacheStatus::Hit, it->second};
  }

  void populate(const UrlRecord& record) override {
    std::lock_guard<std::mutex> lock(mutex_);
    positives[record.short_code] = record;
  }

  void storeCreated(const UrlRecord& record) override {
    std::lock_guard<std::mutex> lock(mutex_);
    positives[record.short_code] = record;
  }

  void storeNegative(const std::string& short_code) override {
    std::lock_guard<std::mutex> lock(mutex_);
    positives.erase(short_code);
    negatives.insert(short_code);
  }

  bool invalidate(const std::string& short_code) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++invalidations;
    if (fail_invalidate) {
      return false;
    }
    positives.erase(short_code);
    negatives.insert(short_code);
    return true;
  }

  bool hasPositive(const std::string& short_code) {
    std::lock_guard<std::mutex> lock(mutex_);
    return positives.contains(short_code);
  }

  std::mutex mutex_;
  std::unordered_map<std::string, UrlRecord> positives;
  std::unordered_set<std::string> negatives;
  bool unavailable{false};
  bool fail_invalidate{false};
  int lookups{0};
  int invalidations{0};
};

struct Fixture {
  Fixture()
      : generator({"abc1234", "def5678", "ghi9012"}),
        service(repository, cache, generator, metrics, logger, 7, 3) {}

  Logger logger{LogLevel::Error};
  Metrics metrics;
  FakeRepository repository;
  FakeCache cache;
  FixedGenerator generator;
  UrlService service;
};

}  // namespace

TEST_CASE("service creates, resolves, reads metadata and deletes") {
  Fixture fixture;

  auto created = fixture.service.create("https://example.com/one", std::nullopt);
  REQUIRE(created.status == CreateOutcome::Status::Created);
  REQUIRE(created.record.short_code == "abc1234");
  REQUIRE(fixture.cache.hasPositive("abc1234"));

  auto resolved = fixture.service.resolve("abc1234");
  REQUIRE(resolved.status == ResolveOutcome::Status::Found);
  REQUIRE(resolved.record.original_url == "https://example.com/one");
  REQUIRE(fixture.repository.find_calls == 0);

  auto metadata = fixture.service.metadata("abc1234");
  REQUIRE(metadata.status == MetadataOutcome::Status::Found);

  auto deleted = fixture.service.remove("abc1234");
  REQUIRE(deleted.status == DeleteOutcome::Status::Deleted);
  REQUIRE_FALSE(fixture.cache.hasPositive("abc1234"));
  REQUIRE(fixture.service.resolve("abc1234").status == ResolveOutcome::Status::NotFound);
  REQUIRE(fixture.service.metadata("abc1234").status == MetadataOutcome::Status::Gone);
}

TEST_CASE("service validates URLs and rejects past expiration") {
  Fixture fixture;

  REQUIRE(fixture.service.create("ftp://example.com", std::nullopt).status ==
          CreateOutcome::Status::ValidationError);
  REQUIRE(fixture.service.create("https://example.com", nowEpochSeconds() - 1).status ==
          CreateOutcome::Status::Expired);
  REQUIRE(fixture.repository.create_calls == 0);
}

TEST_CASE("service retries short code collisions") {
  Fixture fixture;
  fixture.repository.collisions_remaining = 1;

  const auto outcome = fixture.service.create("https://example.com/collision", std::nullopt);
  REQUIRE(outcome.status == CreateOutcome::Status::Created);
  REQUIRE(outcome.record.short_code == "def5678");
  REQUIRE(fixture.repository.create_calls == 2);
  REQUIRE(fixture.generator.calls() == 2);
}

TEST_CASE("service reports allocation exhaustion") {
  Fixture fixture;
  fixture.repository.collisions_remaining = 10;

  const auto outcome = fixture.service.create("https://example.com/exhausted", std::nullopt);
  REQUIRE(outcome.status == CreateOutcome::Status::Exhausted);
  REQUIRE(fixture.repository.create_calls == 3);
}

TEST_CASE("service maps repository failures") {
  Fixture fixture;
  fixture.repository.next_create_status = RepoStatus::Unavailable;
  REQUIRE(fixture.service.create("https://example.com/fail", std::nullopt).status ==
          CreateOutcome::Status::Unavailable);

  fixture.repository.next_find_status = RepoStatus::Unavailable;
  REQUIRE(fixture.service.resolve("missing1").status == ResolveOutcome::Status::Unavailable);

  fixture.repository.next_find_status = RepoStatus::Ok;
  REQUIRE(fixture.service.metadata("missing1").status == MetadataOutcome::Status::NotFound);

  fixture.repository.next_deactivate_status = RepoStatus::Unavailable;
  REQUIRE(fixture.service.remove("missing1").status == DeleteOutcome::Status::Unavailable);
}

TEST_CASE("service caches negative lookups and expired records") {
  Fixture fixture;
  UrlRecord expired;
  expired.short_code = "expired1";
  expired.original_url = "https://example.com/expired";
  expired.created_epoch = nowEpochSeconds() - 100;
  expired.expires_epoch = nowEpochSeconds() - 1;
  expired.is_active = true;
  fixture.repository.rows[expired.short_code] = expired;

  REQUIRE(fixture.service.resolve(expired.short_code).status ==
          ResolveOutcome::Status::NotFound);
  REQUIRE(fixture.cache.lookup(expired.short_code).status == CacheStatus::Negative);
  REQUIRE(fixture.service.resolve(expired.short_code).status ==
          ResolveOutcome::Status::NotFound);
  REQUIRE(fixture.repository.find_calls == 1);
}

TEST_CASE("delete reports failure when cache revocation cannot be written") {
  Fixture fixture;
  auto created = fixture.service.create("https://example.com/revoke", std::nullopt);
  REQUIRE(created.status == CreateOutcome::Status::Created);

  fixture.cache.fail_invalidate = true;
  const auto failed = fixture.service.remove(created.record.short_code);
  REQUIRE(failed.status == DeleteOutcome::Status::Unavailable);
  REQUIRE(fixture.repository.rows[created.record.short_code].is_active == false);
  REQUIRE(fixture.cache.hasPositive(created.record.short_code));

  fixture.cache.fail_invalidate = false;
  REQUIRE(fixture.service.remove(created.record.short_code).status ==
          DeleteOutcome::Status::Deleted);
  REQUIRE_FALSE(fixture.cache.hasPositive(created.record.short_code));
}

TEST_CASE("service coalesces concurrent cold-cache misses") {
  Fixture fixture;
  UrlRecord record;
  record.id = 10;
  record.short_code = "cold1234";
  record.original_url = "https://example.com/cold";
  record.created_epoch = nowEpochSeconds();
  record.is_active = true;
  fixture.repository.rows[record.short_code] = record;
  fixture.repository.delay_find = 100ms;

  constexpr int kThreads = 8;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      const auto outcome = fixture.service.resolve(record.short_code);
      if (outcome.status != ResolveOutcome::Status::Found) {
        failures.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  REQUIRE(failures.load() == 0);
  REQUIRE(fixture.repository.find_calls == 1);
}
