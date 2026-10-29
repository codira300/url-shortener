#pragma once

#include <string>
#include <string_view>

namespace urlshort {

using SV = std::string_view;

enum class RouteKind {
  Redirect,
  Create,
  Metadata,
  Delete,
  HealthLive,
  HealthReady,
  Metrics,
  NotFound,
  MethodNotAllowed,
};

struct RouteMatch {
  RouteKind kind{RouteKind::NotFound};
  std::string short_code;
  std::string allowed_methods;
};

RouteMatch routeRequest(std::string_view method, std::string_view target);

}  // namespace urlshort