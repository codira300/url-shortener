#pragma once

#include "app/UrlService.h"
#include "cache/RedisGateway.h"
#include "cache/RedisUrlCache.h"
#include "config/Config.h"
#include "db/PgPool.h"
#include "db/PostgresUrlRepository.h"
#include "domain/ShortCodeGenerator.h"
#include "http/HttpMessages.h"
#include "http/HttpServer.h"
#include "http/Router.h"
#include "net/ClientIdentity.h"
#include "observability/HealthService.h"
#include "observability/Metrics.h"
#include "ratelimit/RateLimiter.h"
#include "util/Logger.h"
#include "util/ThreadPool.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace urlshort {

class Application {
 public:
  explicit Application(Config config);
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  void run();

 private:
  HttpResponse handle(const HttpRequest& request);
  HttpResponse routeAndHandle(const HttpRequest& request, Route& route);
  std::optional<HttpResponse> enforceRateLimit(std::string_view scope,
                                               const std::string& identity, int capacity,
                                               int refill_per_second, bool fail_open);
  std::string clientIdentity(const HttpRequest& request) const;

  Config config_;
  Logger logger_;
  Metrics metrics_;
  TrustedProxyConfig trusted_proxies_;
  PgPool pg_pool_;
  RedisGateway redis_;
  RandomShortCodeGenerator generator_;
  PostgresUrlRepository repository_;
  RedisUrlCache cache_;
  RedisRateLimiter limiter_;
  UrlService service_;
  HealthService health_;
  ThreadPool workers_;
  HttpServer http_;

  std::atomic<std::int64_t> active_requests_{0};
};

}  // namespace urlshort
