#include "observability/Metrics.h"

#include <array>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>

namespace urlshort {
namespace {

constexpr std::array<int, Metrics::kStatusCount> kStatuses = {
    200, 201, 204, 301, 302, 400, 404, 405, 409, 413, 422, 429, 500, 503, 0};

constexpr std::array<std::string_view, Metrics::kStatusCount> kStatusLabels = {
    "200", "201", "204", "301", "302", "400", "404", "405",
    "409", "413", "422", "429", "500", "503", "other"};

// Upper bounds are expressed in microseconds and are cumulative, matching the
// Prometheus histogram convention.
constexpr std::array<std::uint64_t, Metrics::kBucketCount - 1> kBucketBoundsUs = {
    100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000, 250000, 500000};

constexpr std::array<std::string_view, Metrics::kBucketCount - 1> kBucketLabels = {
    "0.0001", "0.00025", "0.0005", "0.001", "0.0025", "0.005",
    "0.01",   "0.025",   "0.05",   "0.1",   "0.25",   "0.5"};

constexpr std::array<std::string_view, Metrics::kRouteCount> kRouteNames = {
    "redirect", "create", "metadata", "delete", "health", "metrics", "not_found", "other"};

constexpr std::array<std::string_view, 3> kScopeNames = {"redirect", "create", "admin"};

std::size_t scopeIndex(std::string_view scope) {
  for (std::size_t i = 0; i < kScopeNames.size(); ++i) {
    if (scope == kScopeNames[i]) {
      return i;
    }
  }
  return 2;
}

}  // namespace

const char* routeName(Route route) {
  const auto index = static_cast<std::size_t>(route);
  return index < kRouteNames.size() ? kRouteNames[index].data() : "other";
}

Metrics::Histogram::Histogram() {
  for (auto& bucket : buckets) {
    bucket.store(0, std::memory_order_relaxed);
  }
  count.store(0, std::memory_order_relaxed);
  sum_us.store(0, std::memory_order_relaxed);
}

Metrics::Metrics() {
  for (auto& row : requests_) {
    for (auto& value : row) {
      value.store(0, std::memory_order_relaxed);
    }
  }
}

std::size_t Metrics::statusIndex(int status) {
  for (std::size_t i = 0; i + 1 < kStatuses.size(); ++i) {
    if (kStatuses[i] == status) {
      return i;
    }
  }
  return kStatuses.size() - 1;
}

void Metrics::incScopeCounter(std::array<std::atomic<std::uint64_t>, 3>& counters,
                              std::string_view scope) {
  counters[scopeIndex(scope)].fetch_add(1, std::memory_order_relaxed);
}

void Metrics::observe(Histogram& histogram, std::uint64_t duration_us) {
  for (std::size_t i = 0; i < kBucketBoundsUs.size(); ++i) {
    if (duration_us <= kBucketBoundsUs[i]) {
      histogram.buckets[i].fetch_add(1, std::memory_order_relaxed);
    }
  }
  histogram.buckets[kBucketCount - 1].fetch_add(1, std::memory_order_relaxed);
  histogram.count.fetch_add(1, std::memory_order_relaxed);
  histogram.sum_us.fetch_add(duration_us, std::memory_order_relaxed);
}

void Metrics::recordRequest(Route route, int status, std::uint64_t duration_us) {
  const auto route_index = static_cast<std::size_t>(route);
  if (route_index >= kRouteCount) {
    return;
  }
  requests_[route_index][statusIndex(status)].fetch_add(1, std::memory_order_relaxed);
  observe(histograms_[route_index], duration_us);
}

void Metrics::recordDbQuery(std::uint64_t duration_us) {
  observe(db_query_histogram_, duration_us);
}

void Metrics::recordRedisCommand(std::uint64_t duration_us) {
  observe(redis_command_histogram_, duration_us);
}

void Metrics::recordDbPoolWait(std::uint64_t duration_us) {
  observe(db_pool_wait_histogram_, duration_us);
}

void Metrics::incCacheHit() { cache_hits_.fetch_add(1, std::memory_order_relaxed); }
void Metrics::incCacheMiss() { cache_misses_.fetch_add(1, std::memory_order_relaxed); }
void Metrics::incCacheNegativeHit() {
  cache_negative_hits_.fetch_add(1, std::memory_order_relaxed);
}
void Metrics::incCacheUnavailable() {
  cache_unavailable_.fetch_add(1, std::memory_order_relaxed);
}
void Metrics::incCacheTombstonePrevented() {
  cache_tombstone_prevented_.fetch_add(1, std::memory_order_relaxed);
}
void Metrics::incRateLimitAllowed(std::string_view scope) {
  incScopeCounter(rate_limit_allowed_, scope);
}
void Metrics::incRateLimitRejected(std::string_view scope) {
  incScopeCounter(rate_limit_rejected_, scope);
}
void Metrics::incRateLimitUnavailable(std::string_view scope) {
  incScopeCounter(rate_limit_unavailable_, scope);
}
void Metrics::incDbPoolAcquireTimeout() {
  db_pool_acquire_timeouts_.fetch_add(1, std::memory_order_relaxed);
}
void Metrics::incDbError() { db_errors_.fetch_add(1, std::memory_order_relaxed); }
void Metrics::incRedisError() { redis_errors_.fetch_add(1, std::memory_order_relaxed); }
void Metrics::incRedisCircuitOpen() {
  redis_circuit_open_.fetch_add(1, std::memory_order_relaxed);
}
void Metrics::incHttpParseError() {
  http_parse_errors_.fetch_add(1, std::memory_order_relaxed);
}
void Metrics::incRequestQueueRejection() {
  request_queue_rejections_.fetch_add(1, std::memory_order_relaxed);
}
void Metrics::incSingleflightJoined() {
  singleflight_joined_.fetch_add(1, std::memory_order_relaxed);
}
void Metrics::incUrlCreated() { urls_created_.fetch_add(1, std::memory_order_relaxed); }
void Metrics::incUrlDeleted() { urls_deleted_.fetch_add(1, std::memory_order_relaxed); }
void Metrics::incRedirectServed() { redirects_served_.fetch_add(1, std::memory_order_relaxed); }
void Metrics::incCodeCollision() { code_collisions_.fetch_add(1, std::memory_order_relaxed); }

void Metrics::setActiveRequests(std::int64_t value) {
  active_requests_.store(value, std::memory_order_relaxed);
}
void Metrics::setActiveConnections(std::int64_t value) {
  active_connections_.store(value, std::memory_order_relaxed);
}
void Metrics::setWorkerQueueDepth(std::int64_t value) {
  worker_queue_depth_.store(value, std::memory_order_relaxed);
}
void Metrics::setDbPool(std::int64_t in_use, std::int64_t idle) {
  db_pool_in_use_.store(in_use, std::memory_order_relaxed);
  db_pool_idle_.store(idle, std::memory_order_relaxed);
}
void Metrics::setProcess(std::uint64_t cpu_ms, std::uint64_t rss_bytes, std::int64_t threads) {
  process_cpu_ms_.store(cpu_ms, std::memory_order_relaxed);
  process_rss_bytes_.store(rss_bytes, std::memory_order_relaxed);
  process_threads_.store(threads, std::memory_order_relaxed);
}
void Metrics::setReady(bool value) {
  ready_.store(value ? 1 : 0, std::memory_order_relaxed);
}

std::string Metrics::renderPrometheus() const {
  std::ostringstream out;
  const auto load = [](const auto& value) {
    return value.load(std::memory_order_relaxed);
  };

  out << "# HELP urlshort_http_requests_total HTTP requests completed by route and status.\n";
  out << "# TYPE urlshort_http_requests_total counter\n";
  for (std::size_t route = 0; route < kRouteCount; ++route) {
    for (std::size_t status = 0; status < kStatusCount; ++status) {
      const auto value = load(requests_[route][status]);
      if (value == 0) {
        continue;
      }
      out << "urlshort_http_requests_total{route=\"" << kRouteNames[route] << "\",status=\""
          << kStatusLabels[status] << "\"} " << value << "\n";
    }
  }

  out << "# HELP urlshort_http_request_duration_seconds Request handling duration.\n";
  out << "# TYPE urlshort_http_request_duration_seconds histogram\n";
  for (std::size_t route = 0; route < kRouteCount; ++route) {
    const auto& histogram = histograms_[route];
    for (std::size_t bucket = 0; bucket < kBucketCount - 1; ++bucket) {
      out << "urlshort_http_request_duration_seconds_bucket{route=\"" << kRouteNames[route]
          << "\",le=\"" << kBucketLabels[bucket] << "\"} "
          << load(histogram.buckets[bucket]) << "\n";
    }
    out << "urlshort_http_request_duration_seconds_bucket{route=\"" << kRouteNames[route]
        << "\",le=\"+Inf\"} " << load(histogram.buckets[kBucketCount - 1]) << "\n";
    out << "urlshort_http_request_duration_seconds_count{route=\"" << kRouteNames[route] << "\"} "
        << load(histogram.count) << "\n";
    const double sum_seconds =
        static_cast<double>(load(histogram.sum_us)) / 1'000'000.0;
    char sum_buffer[64];
    std::snprintf(sum_buffer, sizeof(sum_buffer), "%.6f", sum_seconds);
    out << "urlshort_http_request_duration_seconds_sum{route=\"" << kRouteNames[route] << "\"} "
        << sum_buffer << "\n";
  }

  const auto renderDependencyHistogram = [&](std::string_view name, const Histogram& histogram) {
    out << "# HELP " << name << " Dependency operation duration in seconds.\n";
    out << "# TYPE " << name << " histogram\n";
    for (std::size_t bucket = 0; bucket < kBucketCount - 1; ++bucket) {
      out << name << "_bucket{le=\"" << kBucketLabels[bucket] << "\"} "
          << load(histogram.buckets[bucket]) << "\n";
    }
    out << name << "_bucket{le=\"+Inf\"} " << load(histogram.buckets[kBucketCount - 1])
        << "\n";
    out << name << "_count " << load(histogram.count) << "\n";
    const double sum_seconds = static_cast<double>(load(histogram.sum_us)) / 1'000'000.0;
    char sum_buffer[64];
    std::snprintf(sum_buffer, sizeof(sum_buffer), "%.6f", sum_seconds);
    out << name << "_sum " << sum_buffer << "\n";
  };

  renderDependencyHistogram("urlshort_db_query_duration_seconds", db_query_histogram_);
  renderDependencyHistogram("urlshort_redis_command_duration_seconds",
                            redis_command_histogram_);
  renderDependencyHistogram("urlshort_db_pool_wait_seconds", db_pool_wait_histogram_);

  out << "# TYPE urlshort_cache_hits_total counter\n";
  out << "urlshort_cache_hits_total " << load(cache_hits_) << "\n";
  out << "# TYPE urlshort_cache_misses_total counter\n";
  out << "urlshort_cache_misses_total " << load(cache_misses_) << "\n";
  out << "# TYPE urlshort_cache_negative_hits_total counter\n";
  out << "urlshort_cache_negative_hits_total " << load(cache_negative_hits_) << "\n";
  out << "# TYPE urlshort_cache_unavailable_total counter\n";
  out << "urlshort_cache_unavailable_total " << load(cache_unavailable_) << "\n";
  out << "# TYPE urlshort_cache_tombstone_prevented_total counter\n";
  out << "urlshort_cache_tombstone_prevented_total " << load(cache_tombstone_prevented_) << "\n";

  out << "# TYPE urlshort_rate_limit_allowed_total counter\n";
  for (std::size_t i = 0; i < kScopeNames.size(); ++i) {
    out << "urlshort_rate_limit_allowed_total{scope=\"" << kScopeNames[i] << "\"} "
        << load(rate_limit_allowed_[i]) << "\n";
  }
  out << "# TYPE urlshort_rate_limit_rejected_total counter\n";
  for (std::size_t i = 0; i < kScopeNames.size(); ++i) {
    out << "urlshort_rate_limit_rejected_total{scope=\"" << kScopeNames[i] << "\"} "
        << load(rate_limit_rejected_[i]) << "\n";
  }
  out << "# TYPE urlshort_rate_limit_unavailable_total counter\n";
  for (std::size_t i = 0; i < kScopeNames.size(); ++i) {
    out << "urlshort_rate_limit_unavailable_total{scope=\"" << kScopeNames[i] << "\"} "
        << load(rate_limit_unavailable_[i]) << "\n";
  }

  const std::array<std::pair<std::string_view, std::uint64_t>, 10> counters = {{
      {"urlshort_db_pool_acquire_timeouts_total", load(db_pool_acquire_timeouts_)},
      {"urlshort_db_errors_total", load(db_errors_)},
      {"urlshort_redis_errors_total", load(redis_errors_)},
      {"urlshort_redis_circuit_open_total", load(redis_circuit_open_)},
      {"urlshort_http_parse_errors_total", load(http_parse_errors_)},
      {"urlshort_request_queue_rejections_total", load(request_queue_rejections_)},
      {"urlshort_singleflight_joined_total", load(singleflight_joined_)},
      {"urlshort_urls_created_total", load(urls_created_)},
      {"urlshort_urls_deleted_total", load(urls_deleted_)},
      {"urlshort_redirects_served_total", load(redirects_served_)},
  }};
  for (const auto& [name, value] : counters) {
    out << "# TYPE " << name << " counter\n";
    out << name << " " << value << "\n";
  }
  out << "# TYPE urlshort_code_collisions_total counter\n";
  out << "urlshort_code_collisions_total " << load(code_collisions_) << "\n";

  out << "# TYPE urlshort_active_requests gauge\n";
  out << "urlshort_active_requests " << load(active_requests_) << "\n";
  out << "# TYPE urlshort_active_connections gauge\n";
  out << "urlshort_active_connections " << load(active_connections_) << "\n";
  out << "# TYPE urlshort_worker_queue_depth gauge\n";
  out << "urlshort_worker_queue_depth " << load(worker_queue_depth_) << "\n";
  out << "# TYPE urlshort_db_pool_in_use gauge\n";
  out << "urlshort_db_pool_in_use " << load(db_pool_in_use_) << "\n";
  out << "# TYPE urlshort_db_pool_idle gauge\n";
  out << "urlshort_db_pool_idle " << load(db_pool_idle_) << "\n";
  out << "# TYPE urlshort_process_cpu_seconds_total counter\n";
  char cpu_buffer[64];
  std::snprintf(cpu_buffer, sizeof(cpu_buffer), "%.3f",
                static_cast<double>(load(process_cpu_ms_)) / 1000.0);
  out << "urlshort_process_cpu_seconds_total " << cpu_buffer << "\n";
  out << "# TYPE urlshort_process_resident_memory_bytes gauge\n";
  out << "urlshort_process_resident_memory_bytes " << load(process_rss_bytes_) << "\n";
  out << "# TYPE urlshort_process_threads gauge\n";
  out << "urlshort_process_threads " << load(process_threads_) << "\n";
  out << "# TYPE urlshort_ready gauge\n";
  out << "urlshort_ready " << load(ready_) << "\n";
  return out.str();
}

}  // namespace urlshort
