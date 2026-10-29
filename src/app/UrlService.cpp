#include "app/UrlService.h"

#include "cache/RedisUrlCache.h"
#include "db/UrlRepository.h"
#include "domain/ShortCode.h"
#include "domain/ShortCodeGenerator.h"
#include "domain/UrlValidator.h"
#include "observability/Metrics.h"
#include "util/Logger.h"
#include "util/Time.h"

#include <chrono>
#include <exception>
#include <string>
#include <utility>

namespace urlshort {
namespace {

constexpr std::chrono::milliseconds kSingleFlightWait{250};

}  // namespace

UrlService::UrlService(UrlRepository& repository, UrlCache& cache, ShortCodeGenerator& generator,
                       Metrics& metrics, Logger& logger, std::size_t code_length,
                       int max_code_retries)
    : repository_(repository),
      cache_(cache),
      generator_(generator),
      metrics_(metrics),
      logger_(logger),
      code_length_(code_length),
      max_code_retries_(max_code_retries) {}

CreateOutcome UrlService::create(const std::string& original_url,
                                 std::optional<std::int64_t> expires_epoch) {
  const auto validation = validateHttpUrl(original_url);
  if (!validation.valid) {
    return CreateOutcome{CreateOutcome::Status::ValidationError, {}, validation.reason};
  }

  const std::int64_t now = nowEpochSeconds();
  if (expires_epoch.has_value() && *expires_epoch <= now) {
    return CreateOutcome{CreateOutcome::Status::Expired, {},
                         "expires_at must be in the future"};
  }

  for (int attempt = 0; attempt < max_code_retries_; ++attempt) {
    std::string code = generator_.generate(code_length_);
    if (ShortCode::isReserved(code)) {
      continue;
    }

    CreateUrlInput input;
    input.short_code = code;
    input.original_url = original_url;
    input.expires_epoch = expires_epoch;

    auto result = repository_.create(input);
    switch (result.status) {
      case RepoStatus::Ok:
        metrics_.incUrlCreated();
        cache_.storeCreated(result.record);
        return CreateOutcome{CreateOutcome::Status::Created, std::move(result.record), {}};
      case RepoStatus::Collision:
        metrics_.incCodeCollision();
        logger_.warn("short_code_collision", {{"attempt", std::to_string(attempt + 1)}});
        continue;
      case RepoStatus::Unavailable:
        return CreateOutcome{CreateOutcome::Status::Unavailable, {}, result.detail};
      case RepoStatus::Internal:
      case RepoStatus::NotFound:
        return CreateOutcome{CreateOutcome::Status::Internal, {}, result.detail};
    }
  }

  logger_.error("short_code_allocation_exhausted",
                {{"attempts", std::to_string(max_code_retries_)}});
  return CreateOutcome{CreateOutcome::Status::Exhausted, {},
                       "could not allocate a unique short code"};
}

ResolveOutcome UrlService::loadFromDatabase(const std::string& short_code) {
  auto result = repository_.findByCode(short_code);
  switch (result.status) {
    case RepoStatus::Ok: {
      if (!result.record.is_active || result.record.isExpired(nowEpochSeconds())) {
        cache_.storeNegative(short_code);
        return ResolveOutcome{ResolveOutcome::Status::NotFound, {}, {}};
      }
      cache_.populate(result.record);
      return ResolveOutcome{ResolveOutcome::Status::Found, std::move(result.record), {}};
    }
    case RepoStatus::NotFound:
      cache_.storeNegative(short_code);
      return ResolveOutcome{ResolveOutcome::Status::NotFound, {}, {}};
    case RepoStatus::Unavailable:
      return ResolveOutcome{ResolveOutcome::Status::Unavailable, {}, result.detail};
    case RepoStatus::Internal:
    case RepoStatus::Collision:
      return ResolveOutcome{ResolveOutcome::Status::Internal, {}, result.detail};
  }
  return ResolveOutcome{ResolveOutcome::Status::Internal, {}, "unreachable repository state"};
}

ResolveOutcome UrlService::resolve(const std::string& short_code) {
  auto cached = cache_.lookup(short_code);
  switch (cached.status) {
    case CacheStatus::Hit:
      return ResolveOutcome{ResolveOutcome::Status::Found, std::move(*cached.record), {}};
    case CacheStatus::Negative:
      return ResolveOutcome{ResolveOutcome::Status::NotFound, {}, {}};
    case CacheStatus::Miss:
    case CacheStatus::Unavailable:
      break;
  }

  try {
    auto flight = resolve_flight_.execute(
        short_code, [this, &short_code] { return loadFromDatabase(short_code); },
        kSingleFlightWait);
    if (flight.timed_out) {
      return ResolveOutcome{ResolveOutcome::Status::Unavailable, {},
                            "single-flight wait timed out"};
    }
    if (!flight.leader) {
      metrics_.incSingleflightJoined();
    }
    if (flight.value.has_value()) {
      return std::move(*flight.value);
    }
    return ResolveOutcome{ResolveOutcome::Status::Internal, {}, "empty single-flight result"};
  } catch (const std::exception& error) {
    logger_.error("resolve_singleflight_error", {{"detail", error.what()}});
    return ResolveOutcome{ResolveOutcome::Status::Internal, {}, "resolve failed"};
  } catch (...) {
    logger_.error("resolve_singleflight_error", {{"detail", "unknown exception"}});
    return ResolveOutcome{ResolveOutcome::Status::Internal, {}, "resolve failed"};
  }
}

MetadataOutcome UrlService::metadata(const std::string& short_code) {
  auto result = repository_.findByCode(short_code);
  switch (result.status) {
    case RepoStatus::Ok:
      if (!result.record.is_active || result.record.isExpired(nowEpochSeconds())) {
        return MetadataOutcome{MetadataOutcome::Status::Gone, std::move(result.record), {}};
      }
      return MetadataOutcome{MetadataOutcome::Status::Found, std::move(result.record), {}};
    case RepoStatus::NotFound:
      return MetadataOutcome{MetadataOutcome::Status::NotFound, {}, {}};
    case RepoStatus::Unavailable:
      return MetadataOutcome{MetadataOutcome::Status::Unavailable, {}, result.detail};
    case RepoStatus::Internal:
    case RepoStatus::Collision:
      return MetadataOutcome{MetadataOutcome::Status::Internal, {}, result.detail};
  }
  return MetadataOutcome{MetadataOutcome::Status::Internal, {}, "unreachable repository state"};
}

DeleteOutcome UrlService::remove(const std::string& short_code) {
  auto result = repository_.deactivate(short_code);
  if (result.status == RepoStatus::NotFound) {
    return DeleteOutcome{DeleteOutcome::Status::NotFound, {}};
  }
  if (result.status == RepoStatus::Unavailable) {
    return DeleteOutcome{DeleteOutcome::Status::Unavailable, result.detail};
  }
  if (result.status != RepoStatus::Ok) {
    return DeleteOutcome{DeleteOutcome::Status::Internal, result.detail};
  }

  metrics_.incUrlDeleted();
  if (!cache_.invalidate(short_code)) {
    logger_.error("delete_cache_invalidation_failed", {{"short_code", short_code}});
    return DeleteOutcome{DeleteOutcome::Status::Unavailable,
                         "URL is inactive but cache revocation must be retried"};
  }
  return DeleteOutcome{DeleteOutcome::Status::Deleted, {}};
}

}  // namespace urlshort
