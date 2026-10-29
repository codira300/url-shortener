#include "http/Router.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace urlshort;

namespace {

void requireRoute(RouteKind expected, std::string_view method, std::string_view target) {
  const auto match = routeRequest(method, target);
  REQUIRE(match.kind == expected);
}

}  // namespace

TEST_CASE("router matches health and metrics endpoints") {
  requireRoute(RouteKind::HealthLive, "GET", "/health/live");
  requireRoute(RouteKind::HealthReady, "GET", "/health/ready");
  requireRoute(RouteKind::Metrics, "GET", "/metrics");

  const auto health = routeRequest("POST", "/health/live");
  REQUIRE(health.kind == RouteKind::MethodNotAllowed);
  REQUIRE(health.allowed_methods == "GET");

  requireRoute(RouteKind::HealthLive, "GET", "/health/live?probe=1");
}

TEST_CASE("router separates collection and item API routes") {
  requireRoute(RouteKind::Create, "POST", "/api/v1/urls");

  const auto collection_get = routeRequest("GET", "/api/v1/urls");
  REQUIRE(collection_get.kind == RouteKind::MethodNotAllowed);
  REQUIRE(collection_get.allowed_methods == "POST");

  const auto metadata = routeRequest("GET", "/api/v1/urls/abc123");
  REQUIRE(metadata.kind == RouteKind::Metadata);
  REQUIRE(metadata.short_code == "abc123");

  const auto remove = routeRequest("DELETE", "/api/v1/urls/abc123");
  REQUIRE(remove.kind == RouteKind::Delete);
  REQUIRE(remove.short_code == "abc123");

  const auto item_post = routeRequest("POST", "/api/v1/urls/abc123");
  REQUIRE(item_post.kind == RouteKind::MethodNotAllowed);
  REQUIRE(item_post.allowed_methods == "GET, DELETE");

  requireRoute(RouteKind::NotFound, "GET", "/api/v1/urls/");
  requireRoute(RouteKind::NotFound, "GET", "/api/v1/urls/abc/extra");
}

TEST_CASE("router extracts redirect codes from origin-form and absolute-form targets") {
  const auto origin = routeRequest("GET", "/abc123?source=test#fragment");
  REQUIRE(origin.kind == RouteKind::Redirect);
  REQUIRE(origin.short_code == "abc123");

  const auto absolute = routeRequest("GET", "http://example.com/abc123?source=test");
  REQUIRE(absolute.kind == RouteKind::Redirect);
  REQUIRE(absolute.short_code == "abc123");

  const auto root = routeRequest("GET", "http://example.com");
  REQUIRE(root.kind == RouteKind::NotFound);

  const auto method = routeRequest("POST", "/abc123");
  REQUIRE(method.kind == RouteKind::MethodNotAllowed);
  REQUIRE(method.allowed_methods == "GET");

  requireRoute(RouteKind::NotFound, "GET", "/");
  requireRoute(RouteKind::NotFound, "GET", "/abc/extra");
}

TEST_CASE("router rejects absolute-form API paths with encoded separators") {
  const auto path = routeRequest("GET", "https://example.com/api/v1/urls/abc123");
  REQUIRE(path.kind == RouteKind::Metadata);
  REQUIRE(path.short_code == "abc123");
}
