#include "app/Application.h"

#include "domain/ShortCode.h"
#include "util/Time.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace urlshort {

    namespace {
        using json = nlohmann::json;
        using SV   = std::string_view;
        using Clock = std::chrono::steady_clock;

        constexpr SV kJsonContentType = "application/json; charset=utf-8";
        constexpr SV kNoStore = "no-store";

        TrustedProxyConfig makeTrustedProxies(const Config& config) {
            std::string error;
            auto parsed = TrustedProxyConfig::parse(config.trust_proxy, config.trusted_proxy_cidrs, error);
            if (!parsed) {
                throw ConfigError("invalid trusted proxy configuration: " + error);
            }
            return *parsed;
        }

        HttpResponse jsonResponse(int status, const json& body) {
            HttpResponse response;
            response.status = status;
            response.content_type = std::string(kJsonContentType);
            response.body = body.dump();
            response.addHeader("Cache-Control", std::string(kNoStore));
            return response;
        }

        HttpResponse errorResponse(int status, SV code, SV message) {
            json body;
            body["error"]["code"] = code;
            body["error"]["message"] = message;
            auto response = jsonResponse(status, body);
            if (status == 503) {
                response.addHeader("Retry-After", "1");
            }
            return response;
        }

        bool unsafeHeaderValue(SV value) {
            return value.find_first_of("\r\n") != SV::npos;
        }

        Route metricsRoute(RouteKind kind) {
            switch (kind) {
                case RouteKind::Redirect:  return Route::Redirect;
                case RouteKind::Create:    return Route::Create;
                case RouteKind::Metadata:  return Route::Metadata;
                case RouteKind::Delete:    return Route::Delete;
                case RouteKind::HealthLive:
                case RouteKind::HealthReady: return Route::Health;
                case RouteKind::Metrics:   return Route::Metrics;
                case RouteKind::NotFound:  return Route::NotFound;
                case RouteKind::MethodNotAllowed: return Route::Other;
            }
            return Route::Other;
        }

        json recordJson(const UrlRecord& record, SV public_base_url) {
            json body;
            body["short_code"]   = record.short_code;
            body["short_url"]    = std::string(public_base_url) + "/" + record.short_code;
            body["original_url"] = record.original_url;
            body["created_at"]   = formatRfc3339(record.created_epoch);
            body["expires_at"]   = record.expires_epoch.has_value() ? json(formatRfc3339(*record.expires_epoch)) : json(nullptr);
            body["is_active"]    = record.is_active;
            return body;
        }

    }  

    Application::Application(Config config)
        : config_(std::move(config)),
          logger_(parseLogLevel(config_.log.level)),
          metrics_(),
          trusted_proxies_(makeTrustedProxies(config_)),
          pg_pool_(config_.pg, metrics_, logger_),
          redis_(config_.redis, metrics_, logger_),
          generator_(),
          repository_(pg_pool_, metrics_, logger_, std::chrono::milliseconds(config_.pg.acquire_timeout_ms)),
          cache_(redis_, config_.cache, metrics_, logger_),
          limiter_(redis_, metrics_, logger_),
          service_(repository_, cache_, generator_, metrics_, logger_, static_cast<std::size_t>(config_.short_code_length),
                  config_.short_code_max_retries),
          health_(pg_pool_, redis_, metrics_, logger_, std::chrono::milliseconds(config_.readiness_refresh_ms),
                  std::chrono::milliseconds(std::min(config_.pg.acquire_timeout_ms, 1000))),
          workers_(static_cast<std::size_t>(config_.worker.threads),
                  static_cast<std::size_t>(config_.worker.queue_capacity), logger_),
          http_(config_.http, config_.shutdown_grace_ms,
                [this](const HttpRequest& request) { return handle(request); }, workers_, metrics_,
                logger_) 

          {
            logger_.info(
                    "application_configured",
                    {{"http_host", config_.http.host},
                      {"http_port", std::to_string(config_.http.port)},
                      {"io_threads", std::to_string(config_.http.io_threads)},
                      {"worker_threads", std::to_string(config_.worker.threads)},
                      {"worker_queue_capacity", std::to_string(config_.worker.queue_capacity)},
                      {"pg_dsn", redactDsn(config_.pg.dsn)},
                      {"pg_pool_size", std::to_string(config_.pg.pool_size)},
                      {"redis", config_.redis.host + ":" + std::to_string(config_.redis.port)},
                      {"redis_pool_size", std::to_string(config_.redis.pool_size)},
                      {"short_code_length", std::to_string(config_.short_code_length)},
                      {"trust_proxy", config_.trust_proxy ? "true" : "false"}}
                    );
           }

    Application::~Application() {
        try {
            http_.stop();
            workers_.shutdown(std::chrono::milliseconds(config_.shutdown_grace_ms));
            health_.stop();
            metrics_.setReady(false);
            logger_.info("application_destroyed");
        } catch (...) {
            // Destructors must not throw.
        }
    }

    void Application::run() {
        health_.start();
        logger_.info("http_listening", {{"address", config_.http.host + ":" + std::to_string(config_.http.port)}});
        http_.run();
        workers_.shutdown(std::chrono::milliseconds(config_.shutdown_grace_ms));
        health_.stop();
        metrics_.setReady(false);
        logger_.info("application_stopped");
    }

HttpResponse Application::handle(const HttpRequest& request) {
    const auto started = request.received_at == Clock::time_point{}
                             ? Clock::now()
                             : request.received_at;
    const auto previous_active = active_requests_.fetch_add(1, std::memory_order_acq_rel) + 1;
    metrics_.setActiveRequests(previous_active);

    Route route = Route::Other;
    HttpResponse response;
    try {
        response = routeAndHandle(request, route);
    } catch (const std::exception& error) {
        logger_.error("request_failed", {{"detail", error.what()}});
        response = errorResponse(500, "internal_error", "internal server error");
    } catch (...) {
        logger_.error("request_failed", {{"detail", "unknown exception"}});
        response = errorResponse(500, "internal_error", "internal server error");
    }

    const auto active = active_requests_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    metrics_.setActiveRequests(active);
    const auto elapsed = Clock::now() - started;
    const auto duration_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    metrics_.recordRequest(route, response.status, duration_us);
    if (config_.log.access) {
        logger_.info("access",
                     {{"method", request.method},
                      {"target", request.target},
                      {"status", std::to_string(response.status)},
                      {"duration_us", std::to_string(duration_us)},
                      {"route", routeName(route)},
                      {"peer_ip", request.peer_ip}});
    }
    return response;
}

std::string Application::clientIdentity(const HttpRequest& request) const {
    auto identity = ClientIdentity::resolve(request.peer_ip, request.forwarded_for, trusted_proxies_);
    if (identity.empty()) {
        return "unknown";
    }
    return identity;
}

std::optional<HttpResponse> Application::enforceRateLimit(SV scope, const std::string& identity,
                                                          int capacity, int refill_per_second,
                                                          bool fail_open) {
    const auto decision = limiter_.check(scope, identity, capacity, refill_per_second, fail_open);
    if (decision.status == RateLimitStatus::Allowed) {
        return std::nullopt;
    }
    if (decision.status == RateLimitStatus::Limited) {
        const int retry_after_seconds = std::max(1, (decision.retry_after_ms + 999) / 1000);
        auto response = errorResponse(429, "rate_limited", "too many requests");
        response.addHeader("Retry-After", std::to_string(retry_after_seconds));
        return response;
    }
    return errorResponse(503, "rate_limiter_unavailable", "rate limiter unavailable");
}

HttpResponse Application::routeAndHandle(const HttpRequest& request, Route& route) {
    const auto match = routeRequest(request.method, request.target);
    route = metricsRoute(match.kind);

    switch (match.kind) {
        case RouteKind::MethodNotAllowed: {
            auto response =
                errorResponse(405, "method_not_allowed", "method not allowed for this path");
            response.addHeader("Allow", match.allowed_methods);
            return response;
        }
        case RouteKind::NotFound:
            return errorResponse(404, "not_found", "resource not found");
        case RouteKind::HealthLive: {
            json body;
            body["status"] = "ok";
            return jsonResponse(200, body);
        }
        case RouteKind::HealthReady: {
            const bool pg_ready = health_.postgresReady();
            const bool redis_ready = health_.redisReady();
            json body;
            body["status"]   = health_.ready() ? "ready" : "not_ready";
            body["postgres"] = pg_ready ? "up" : "down";
            body["redis"]    = redis_ready ? "up" : "down";
            body["degraded"] = pg_ready && !redis_ready;
            auto response = jsonResponse(health_.ready() ? 200 : 503, body);
            response.addHeader("Cache-Control", std::string(kNoStore));
            return response;
        }
        case RouteKind::Metrics: {
            HttpResponse response;
            response.status = 200;
            response.content_type = "text/plain; version=0.0.4; charset=utf-8";
            response.body = metrics_.renderPrometheus();
            response.addHeader("Cache-Control", std::string(kNoStore));
            return response;
        }
        case RouteKind::Redirect: {
            if (!ShortCode::isValid(match.short_code) ||
                ShortCode::isReserved(match.short_code)) {
                return errorResponse(404, "not_found", "short code not found");
            }
            if (auto limited = enforceRateLimit(
                    "redirect", clientIdentity(request), config_.rate_limit.redirect_burst,
                    config_.rate_limit.redirect_rps, config_.rate_limit.fail_open_redirect)) {
                return *limited;
            }

            const auto outcome = service_.resolve(match.short_code);
            switch (outcome.status) {
                case ResolveOutcome::Status::Found: {
                    if (unsafeHeaderValue(outcome.record.original_url)) {
                        logger_.error("unsafe_redirect_target");
                        return errorResponse(500, "internal_error",
                                             "stored destination is invalid");
                    }
                    metrics_.incRedirectServed();
                    HttpResponse response;
                    response.status = 302;
                    response.content_type = "text/plain; charset=utf-8";
                    response.body = "";
                    response.addHeader("Location", outcome.record.original_url);
                    response.addHeader("Cache-Control", std::string(kNoStore));
                    response.addHeader("Referrer-Policy", "no-referrer");
                    return response;
                }
                case ResolveOutcome::Status::NotFound:
                    return errorResponse(404, "not_found", "short code not found");
                case ResolveOutcome::Status::Unavailable:
                    return errorResponse(503, "database_unavailable",
                                         "temporary service unavailability");
                case ResolveOutcome::Status::Internal:
                    return errorResponse(500, "internal_error", "internal server error");
            }
            return errorResponse(500, "internal_error", "internal server error");
        }
        case RouteKind::Create: {
            if (auto limited = enforceRateLimit("create", clientIdentity(request),
                                                config_.rate_limit.create_burst,
                                                config_.rate_limit.create_rps, false)) {
                return *limited;
            }

            json body;
            try {
                body = json::parse(request.body);
            } catch (const json::exception&) {
                return errorResponse(400, "invalid_json", "request body must be valid JSON");
            }
            if (!body.is_object()) {
                return errorResponse(400, "invalid_json",
                                     "request body must be a JSON object");
            }
            const auto url_it = body.find("url");
            if (url_it == body.end() || !url_it->is_string()) {
                return errorResponse(422, "invalid_url", "url must be a string");
            }
            const auto url = url_it->get<std::string>();

            std::optional<std::int64_t> expires_epoch;
            const auto expires_it = body.find("expires_at");
            if (expires_it != body.end() && !expires_it->is_null()) {
                if (!expires_it->is_string()) {
                    return errorResponse(422, "invalid_expires_at",
                                         "expires_at must be an RFC 3339 string or null");
                }
                const auto parsed = parseRfc3339(expires_it->get<std::string>());
                if (!parsed) {
                    return errorResponse(422, "invalid_expires_at",
                                         "expires_at must be an RFC 3339 timestamp with a timezone");
                }
                expires_epoch = *parsed;
            }

            const auto outcome = service_.create(url, expires_epoch);
            switch (outcome.status) {
                case CreateOutcome::Status::Created: {
                    auto response =
                        jsonResponse(201, recordJson(outcome.record, config_.public_base_url));
                    response.addHeader("Location", config_.public_base_url + "/" +
                                                       outcome.record.short_code);
                    return response;
                }
                case CreateOutcome::Status::ValidationError:
                    return errorResponse(422, "invalid_url", outcome.detail);
                case CreateOutcome::Status::Expired:
                    return errorResponse(422, "invalid_expires_at", outcome.detail);
                case CreateOutcome::Status::Exhausted:
                    return errorResponse(503, "code_allocation_failed",
                                         "could not allocate a short code");
                case CreateOutcome::Status::Unavailable:
                    return errorResponse(503, "database_unavailable",
                                         "temporary service unavailability");
                case CreateOutcome::Status::Internal:
                    return errorResponse(500, "internal_error", "internal server error");
            }
            return errorResponse(500, "internal_error", "internal server error");
        }
        case RouteKind::Metadata: {
            if (!ShortCode::isValid(match.short_code) ||
                ShortCode::isReserved(match.short_code)) {
                return errorResponse(404, "not_found", "short code not found");
            }
            if (auto limited = enforceRateLimit("admin", clientIdentity(request),
                                                config_.rate_limit.admin_burst,
                                                config_.rate_limit.admin_rps, false)) {
                return *limited;
            }

            const auto outcome = service_.metadata(match.short_code);
            switch (outcome.status) {
                case MetadataOutcome::Status::Found:
                    return jsonResponse(200,
                                        recordJson(outcome.record, config_.public_base_url));
                case MetadataOutcome::Status::NotFound:
                    return errorResponse(404, "not_found", "short code not found");
                case MetadataOutcome::Status::Gone:
                    return errorResponse(410, "gone", "short code is inactive or expired");
                case MetadataOutcome::Status::Unavailable:
                    return errorResponse(503, "database_unavailable",
                                         "temporary service unavailability");
                case MetadataOutcome::Status::Internal:
                    return errorResponse(500, "internal_error", "internal server error");
            }
            return errorResponse(500, "internal_error", "internal server error");
        }
        case RouteKind::Delete: {
            if (!ShortCode::isValid(match.short_code) ||
                ShortCode::isReserved(match.short_code)) {
                return errorResponse(404, "not_found", "short code not found");
            }
            if (auto limited = enforceRateLimit("admin", clientIdentity(request),
                                                config_.rate_limit.admin_burst,
                                                config_.rate_limit.admin_rps, false)) {
                return *limited;
            }

            const auto outcome = service_.remove(match.short_code);
            switch (outcome.status) {
                case DeleteOutcome::Status::Deleted: {
                    HttpResponse response;
                    response.status = 204;
                    response.content_type = std::string(kJsonContentType);
                    response.body.clear();
                    response.addHeader("Cache-Control", std::string(kNoStore));
                    return response;
                }
                case DeleteOutcome::Status::NotFound:
                    return errorResponse(404, "not_found", "short code not found");
                case DeleteOutcome::Status::Unavailable:
                    return errorResponse(503, "database_unavailable",
                                         "temporary service unavailability");
                case DeleteOutcome::Status::Internal:
                    return errorResponse(500, "internal_error", "internal server error");
            }
            return errorResponse(500, "internal_error", "internal server error");
        }
    }
    return errorResponse(500, "internal_error", "internal server error");
}

} 