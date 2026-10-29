#include "observability/Metrics.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

using namespace urlshort;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("route names are stable") {
  REQUIRE(std::string(routeName(Route::Redirect)) == "redirect");
  REQUIRE(std::string(routeName(Route::Create)) == "create");
  REQUIRE(std::string(routeName(Route::Metadata)) == "metadata");
  REQUIRE(std::string(routeName(Route::Delete)) == "delete");
  REQUIRE(std::string(routeName(Route::Health)) == "health");
  REQUIRE(std::string(routeName(Route::Metrics)) == "metrics");
  REQUIRE(std::string(routeName(Route::NotFound)) == "not_found");
  REQUIRE(std::string(routeName(Route::Other)) == "other");
}

TEST_CASE("fresh metrics render every series") {
  Metrics metrics;
  const std::string text = metrics.renderPrometheus();

  REQUIRE(contains(text, "# TYPE urlshort_http_requests_total counter"));
  REQUIRE(contains(text, "urlshort_cache_hits_total 0"));
  REQUIRE(contains(text, "urlshort_cache_misses_total 0"));
  REQUIRE(contains(text, "urlshort_urls_created_total 0"));
  REQUIRE(contains(text, "urlshort_redirects_served_total 0"));
  REQUIRE(contains(text, "urlshort_active_requests 0"));
  REQUIRE(contains(text, "urlshort_ready 0"));
  REQUIRE(contains(text, "le=\"+Inf\""));
  REQUIRE(contains(text, "urlshort_db_query_duration_seconds_count 0"));
  REQUIRE(contains(text, "urlshort_redis_command_duration_seconds_count 0"));
  REQUIRE(contains(text, "urlshort_db_pool_wait_seconds_count 0"));
}

TEST_CASE("dependency histograms are rendered") {
  Metrics metrics;
  metrics.recordDbQuery(150);
  metrics.recordDbQuery(900000);
  metrics.recordRedisCommand(50);
  metrics.recordDbPoolWait(250);

  const std::string text = metrics.renderPrometheus();
  REQUIRE(contains(text, "urlshort_db_query_duration_seconds_count 2"));
  REQUIRE(contains(text, "urlshort_db_query_duration_seconds_sum 0.900150"));
  REQUIRE(contains(text, "urlshort_redis_command_duration_seconds_count 1"));
  REQUIRE(contains(text, "urlshort_db_pool_wait_seconds_count 1"));
}

TEST_CASE("requests are counted by route and status") {
  Metrics metrics;
  metrics.recordRequest(Route::Create, 201, 150);
  metrics.recordRequest(Route::Create, 201, 900000);
  metrics.recordRequest(Route::Redirect, 302, 50);
  metrics.recordRequest(Route::Redirect, 418, 50);

  const std::string text = metrics.renderPrometheus();
  REQUIRE(contains(text,
                   "urlshort_http_requests_total{route=\"create\",status=\"201\"} 2"));
  REQUIRE(contains(text,
                   "urlshort_http_requests_total{route=\"redirect\",status=\"302\"} 1"));
  REQUIRE(contains(text,
                   "urlshort_http_requests_total{route=\"redirect\",status=\"other\"} 1"));
  REQUIRE(contains(text, "urlshort_http_request_duration_seconds_count{route=\"create\"} 2"));
  REQUIRE(contains(text, "urlshort_http_request_duration_seconds_count{route=\"redirect\"} 2"));
  REQUIRE(contains(text, "urlshort_http_request_duration_seconds_sum{route=\"create\"} 0.900150"));
}

TEST_CASE("histogram buckets are cumulative") {
  Metrics metrics;
  metrics.recordRequest(Route::Health, 200, 100);

  const std::string text = metrics.renderPrometheus();
  REQUIRE(contains(text, "le=\"0.0001\"} 1"));
  REQUIRE(contains(text, "le=\"0.00025\"} 1"));
  REQUIRE(contains(text, "le=\"0.5\"} 1"));
}

TEST_CASE("counters and gauges are recorded") {
  Metrics metrics;
  metrics.incCacheHit();
  metrics.incCacheHit();
  metrics.incCacheMiss();
  metrics.incCacheNegativeHit();
  metrics.incCacheUnavailable();
  metrics.incCacheTombstonePrevented();
  metrics.incDbPoolAcquireTimeout();
  metrics.incDbError();
  metrics.incRedisError();
  metrics.incRedisCircuitOpen();
  metrics.incHttpParseError();
  metrics.incRequestQueueRejection();
  metrics.incSingleflightJoined();
  metrics.incUrlCreated();
  metrics.incUrlDeleted();
  metrics.incRedirectServed();
  metrics.incCodeCollision();
  metrics.setActiveRequests(3);
  metrics.setActiveConnections(4);
  metrics.setWorkerQueueDepth(5);
  metrics.setDbPool(6, 7);
  metrics.setProcess(1500, 4096, 8);
  metrics.setReady(true);

  const std::string text = metrics.renderPrometheus();
  REQUIRE(contains(text, "urlshort_cache_hits_total 2"));
  REQUIRE(contains(text, "urlshort_cache_misses_total 1"));
  REQUIRE(contains(text, "urlshort_cache_negative_hits_total 1"));
  REQUIRE(contains(text, "urlshort_cache_unavailable_total 1"));
  REQUIRE(contains(text, "urlshort_cache_tombstone_prevented_total 1"));
  REQUIRE(contains(text, "urlshort_db_pool_acquire_timeouts_total 1"));
  REQUIRE(contains(text, "urlshort_db_errors_total 1"));
  REQUIRE(contains(text, "urlshort_redis_errors_total 1"));
  REQUIRE(contains(text, "urlshort_redis_circuit_open_total 1"));
  REQUIRE(contains(text, "urlshort_http_parse_errors_total 1"));
  REQUIRE(contains(text, "urlshort_request_queue_rejections_total 1"));
  REQUIRE(contains(text, "urlshort_singleflight_joined_total 1"));
  REQUIRE(contains(text, "urlshort_urls_created_total 1"));
  REQUIRE(contains(text, "urlshort_urls_deleted_total 1"));
  REQUIRE(contains(text, "urlshort_redirects_served_total 1"));
  REQUIRE(contains(text, "urlshort_code_collisions_total 1"));
  REQUIRE(contains(text, "urlshort_active_requests 3"));
  REQUIRE(contains(text, "urlshort_active_connections 4"));
  REQUIRE(contains(text, "urlshort_worker_queue_depth 5"));
  REQUIRE(contains(text, "urlshort_db_pool_in_use 6"));
  REQUIRE(contains(text, "urlshort_db_pool_idle 7"));
  REQUIRE(contains(text, "urlshort_process_cpu_seconds_total 1.500"));
  REQUIRE(contains(text, "urlshort_process_resident_memory_bytes 4096"));
  REQUIRE(contains(text, "urlshort_process_threads 8"));
  REQUIRE(contains(text, "urlshort_ready 1"));
}

TEST_CASE("rate limit counters are grouped by scope") {
  Metrics metrics;
  metrics.incRateLimitAllowed("redirect");
  metrics.incRateLimitRejected("create");
  metrics.incRateLimitRejected("create");
  metrics.incRateLimitUnavailable("unexpected");

  const std::string text = metrics.renderPrometheus();
  REQUIRE(contains(text, "urlshort_rate_limit_allowed_total{scope=\"redirect\"} 1"));
  REQUIRE(contains(text, "urlshort_rate_limit_rejected_total{scope=\"create\"} 2"));
  REQUIRE(contains(text, "urlshort_rate_limit_unavailable_total{scope=\"admin\"} 1"));
}
