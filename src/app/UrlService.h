#pragma once

#include "domain/UrlRecord.h"
#include "util/SingleFlight.h"

#include <cstddef>
#include <optional>
#include <string>

namespace urlshort {

class UrlRepository;
class UrlCache;
class ShortCodeGenerator;
class Metrics;
class Logger;

struct CreateOutcome {
  enum class Status { Created, ValidationError, Expired, Exhausted, Unavailable, Internal };
  Status status{Status::Internal};
  UrlRecord record;
  std::string detail;
};

struct ResolveOutcome {
  enum class Status { Found, NotFound, Unavailable, Internal };
  Status status{Status::Internal};
  UrlRecord record;
  std::string detail;
};

struct MetadataOutcome {
  enum class Status { Found, NotFound, Gone, Unavailable, Internal };
  Status status{Status::Internal};
  UrlRecord record;
  std::string detail;
};

struct DeleteOutcome {
  enum class Status { Deleted, NotFound, Unavailable, Internal };
  Status status{Status::Internal};
  std::string detail;
};

// Application service for the URL mapping use cases. It owns no resources:
// repositories, cache, and the code generator are injected and outlive it.
//
// Consistency model:
//  * PostgreSQL is authoritative. Redis entries are reconstructible hints.
//  * Deletion writes a tombstone before returning. A concurrent cache fill
//    refuses to overwrite a tombstone, so the ordinary read-then-delete race
//    cannot resurrect a stale positive entry.
//  * If Redis is unavailable during invalidation, the delete is reported as
//    unavailable. The database row is already inactive, so a retried DELETE
//    converges; a stale positive cache entry may survive until its bounded TTL
//    or the retry, whichever happens first. This is eventual, not strict,
//    revocation.
class UrlService {
 public:
  UrlService(UrlRepository& repository, UrlCache& cache, ShortCodeGenerator& generator,
             Metrics& metrics, Logger& logger, std::size_t code_length, int max_code_retries);

  CreateOutcome create(const std::string& original_url,
                       std::optional<std::int64_t> expires_epoch);
  ResolveOutcome resolve(const std::string& short_code);
  MetadataOutcome metadata(const std::string& short_code);
  DeleteOutcome remove(const std::string& short_code);

 private:
  ResolveOutcome loadFromDatabase(const std::string& short_code);

  UrlRepository& repository_;
  UrlCache& cache_;
  ShortCodeGenerator& generator_;
  Metrics& metrics_;
  Logger& logger_;
  const std::size_t code_length_;
  const int max_code_retries_;
  SingleFlight<ResolveOutcome> resolve_flight_;
};

}  // namespace urlshort
