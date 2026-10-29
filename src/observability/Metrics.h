#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace urlshort {

enum class Route : std::size_t {
  Redirect = 0,
  Create,
  Metadata,
  Delete,
  Health,
  Metrics,
  NotFound,
  Other,
  Count
};

const char* routeName(Route route);

class Metrics {
 public:
  static constexpr std::size_t kRouteCount = static_cast<std::size_t>(Route::Count);
  // The final status slot is "other" so unknown status codes never collide
  // with a real HTTP status label.
  static constexpr std::size_t kStatusCount = 15;
  static constexpr std::size_t kBucketCount = 13;

  Metrics();

  void recordRequest(Route route, int status, std::uint64_t duration_us);
  void recordDbQuery(std::uint64_t duration_us);
  void recordRedisCommand(std::uint64_t duration_us);
  void recordDbPoolWait(std::uint64_t duration_us);

  void incCacheHit();
  void incCacheMiss();
  void incCacheNegativeHit();
  void incCacheUnavailable();
  void incCacheTombstonePrevented();
  void incRateLimitAllowed(std::string_view scope);
  void incRateLimitRejected(std::string_view scope);
  void incRateLimitUnavailable(std::string_view scope);
  void incDbPoolAcquireTimeout();
  void incDbError();
  void incRedisError();
  void incRedisCircuitOpen();
  void incHttpParseError();
  void incRequestQueueRejection();
  void incSingleflightJoined();
  void incUrlCreated();
  void incUrlDeleted();
  void incRedirectServed();
  void incCodeCollision();

  void setActiveRequests(std::int64_t value);
  void setActiveConnections(std::int64_t value);
  void setWorkerQueueDepth(std::int64_t value);
  void setDbPool(std::int64_t in_use, std::int64_t idle);
  void setProcess(std::uint64_t cpu_ms, std::uint64_t rss_bytes, std::int64_t threads);
  void setReady(bool value);

  std::string renderPrometheus() const;

 private:
  struct Histogram {
    std::array<std::atomic<std::uint64_t>, kBucketCount> buckets{};
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> sum_us{0};
    Histogram();
  };

  static std::size_t statusIndex(int status);
  static void observe(Histogram& histogram, std::uint64_t duration_us);
  void incScopeCounter(std::array<std::atomic<std::uint64_t>, 3>& counters, std::string_view scope);

  std::array<std::array<std::atomic<std::uint64_t>, kStatusCount>, kRouteCount> requests_{};
  std::array<Histogram, kRouteCount> histograms_{};
  Histogram db_query_histogram_{};
  Histogram redis_command_histogram_{};
  Histogram db_pool_wait_histogram_{};

  std::atomic<std::uint64_t> cache_hits_{0};
  std::atomic<std::uint64_t> cache_misses_{0};
  std::atomic<std::uint64_t> cache_negative_hits_{0};
  std::atomic<std::uint64_t> cache_unavailable_{0};
  std::atomic<std::uint64_t> cache_tombstone_prevented_{0};
  std::array<std::atomic<std::uint64_t>, 3> rate_limit_allowed_{};
  std::array<std::atomic<std::uint64_t>, 3> rate_limit_rejected_{};
  std::array<std::atomic<std::uint64_t>, 3> rate_limit_unavailable_{};
  std::atomic<std::uint64_t> db_pool_acquire_timeouts_{0};
  std::atomic<std::uint64_t> db_errors_{0};
  std::atomic<std::uint64_t> redis_errors_{0};
  std::atomic<std::uint64_t> redis_circuit_open_{0};
  std::atomic<std::uint64_t> http_parse_errors_{0};
  std::atomic<std::uint64_t> request_queue_rejections_{0};
  std::atomic<std::uint64_t> singleflight_joined_{0};
  std::atomic<std::uint64_t> urls_created_{0};
  std::atomic<std::uint64_t> urls_deleted_{0};
  std::atomic<std::uint64_t> redirects_served_{0};
  std::atomic<std::uint64_t> code_collisions_{0};

  std::atomic<std::int64_t> active_requests_{0};
  std::atomic<std::int64_t> active_connections_{0};
  std::atomic<std::int64_t> worker_queue_depth_{0};
  std::atomic<std::int64_t> db_pool_in_use_{0};
  std::atomic<std::int64_t> db_pool_idle_{0};
  std::atomic<std::uint64_t> process_cpu_ms_{0};
  std::atomic<std::uint64_t> process_rss_bytes_{0};
  std::atomic<std::int64_t> process_threads_{0};
  std::atomic<std::int64_t> ready_{0};
};

}  // namespace urlshort
