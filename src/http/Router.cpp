#include "http/Router.h"

#include <string>
#include <string_view>
#include <utility>

namespace urlshort {
namespace {

constexpr bool startsWith(SV value, SV prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

constexpr bool hasHttpScheme(SV value) {
  return startsWith(value, "http://") || startsWith(value, "https://");
}

// Converts either an origin-form or absolute-form request target into a path.
// Query strings and fragments are not part of routing and are dropped.
SV pathOnly(SV target) {
  if (target.empty()) {
    return target;
  }

  SV path = target;
  if (hasHttpScheme(path)) {
    const auto scheme_end = path.find("://");
    const auto authority_start = scheme_end + 3;
    const auto path_start = path.find_first_of("/?#", authority_start);
    if (path_start == SV::npos) {
      return "/";
    }
    path = path.substr(path_start);
  }

  const auto query = path.find_first_of("?#");
  if (query != SV::npos) {
    path = path.substr(0, query);
  }

  if (path.empty()) {
    return "/";
  }
  return path;
}

constexpr SV kApiCollection = "/api/v1/urls";
constexpr SV kApiItemPrefix = "/api/v1/urls/";

bool isApiUrlCollection(SV path) { return path == kApiCollection; }

bool isApiUrlItem(SV path) {
  if (!startsWith(path, kApiItemPrefix) || path.size() <= kApiItemPrefix.size()) {
    return false;
  }

  const auto code = path.substr(kApiItemPrefix.size());
  return code.find('/') == SV::npos;
}

std::string itemCode(SV path) {
  return std::string(path.substr(kApiItemPrefix.size()));
}

}  // namespace

RouteMatch routeRequest(SV method, SV target) {
  const SV path = pathOnly(target);

  if (path == "/health/live") {
    return method == "GET" ? RouteMatch{RouteKind::HealthLive, {}, {}}
                           : RouteMatch{RouteKind::MethodNotAllowed, {}, "GET"};
  }
  if (path == "/health/ready") {
    return method == "GET" ? RouteMatch{RouteKind::HealthReady, {}, {}}
                           : RouteMatch{RouteKind::MethodNotAllowed, {}, "GET"};
  }
  if (path == "/metrics") {
    return method == "GET" ? RouteMatch{RouteKind::Metrics, {}, {}}
                           : RouteMatch{RouteKind::MethodNotAllowed, {}, "GET"};
  }

  if (isApiUrlCollection(path)) {
    return method == "POST" ? RouteMatch{RouteKind::Create, {}, {}}
                            : RouteMatch{RouteKind::MethodNotAllowed, {}, "POST"};
  }

  if (isApiUrlItem(path)) {
    std::string code = itemCode(path);
    if (method == "GET") {
      return RouteMatch{RouteKind::Metadata, std::move(code), {}};
    }
    if (method == "DELETE") {
      return RouteMatch{RouteKind::Delete, std::move(code), {}};
    }
    return RouteMatch{RouteKind::MethodNotAllowed, std::move(code), "GET, DELETE"};
  }

  if (path.size() > 1 && path.front() == '/') {
    const auto code = path.substr(1);
    if (code.find('/') != SV::npos) {
      return RouteMatch{RouteKind::NotFound, {}, {}};
    }
    if (method == "GET") {
      return RouteMatch{RouteKind::Redirect, std::string(code), {}};
    }
    return RouteMatch{RouteKind::MethodNotAllowed, std::string(code), "GET"};
  }

  return RouteMatch{RouteKind::NotFound, {}, {}};
}

}  // namespace urlshort
