#include "db/PostgresUrlRepository.h"

#include "db/PgPool.h"
#include "observability/Metrics.h"
#include "util/Logger.h"

#include <charconv>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace urlshort {
namespace {

class DbQueryTimer {
 public:
  explicit DbQueryTimer(Metrics& metrics) : metrics_(metrics) {}

  ~DbQueryTimer() {
    const auto elapsed = std::chrono::steady_clock::now() - started_;
    metrics_.recordDbQuery(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
  }

 private:
  Metrics& metrics_;
  const std::chrono::steady_clock::time_point started_{std::chrono::steady_clock::now()};
};

constexpr const char* kCreateSql =
    "INSERT INTO urls (short_code, original_url, expires_at) "
    "VALUES ($1, $2, CASE WHEN $3::text IS NULL THEN NULL "
    "ELSE to_timestamp($3::double precision) END) "
    "RETURNING id, short_code, original_url, is_active, "
    "(extract(epoch from created_at))::bigint, "
    "CASE WHEN expires_at IS NULL THEN NULL ELSE (extract(epoch from expires_at))::bigint END";

constexpr const char* kFindSql =
    "SELECT id, short_code, original_url, is_active, "
    "(extract(epoch from created_at))::bigint, "
    "CASE WHEN expires_at IS NULL THEN NULL ELSE (extract(epoch from expires_at))::bigint END "
    "FROM urls WHERE short_code = $1";

constexpr const char* kDeactivateSql =
    "WITH target AS (SELECT 1 FROM urls WHERE short_code = $1), "
    "updated AS (UPDATE urls SET is_active = FALSE, deactivated_at = now() "
    "WHERE short_code = $1 AND is_active = TRUE RETURNING 1) "
    "SELECT (SELECT count(*) FROM target)::int, (SELECT count(*) FROM updated)::int";

bool parseInt64(std::string_view text, std::int64_t& value) {
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  auto result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end;
}

std::optional<UrlRecord> parseRecord(const PgResult& result, std::string& error) {
  if (result.rows() != 1 || result.columns() < 6) {
    error = "unexpected row shape from PostgreSQL";
    return std::nullopt;
  }

  UrlRecord record;
  if (!parseInt64(result.value(0, 0), record.id)) {
    error = "invalid id from PostgreSQL";
    return std::nullopt;
  }
  record.short_code = std::string(result.value(0, 1));
  record.original_url = std::string(result.value(0, 2));
  record.is_active = result.value(0, 3) == "t";
  if (!parseInt64(result.value(0, 4), record.created_epoch)) {
    error = "invalid created_at from PostgreSQL";
    return std::nullopt;
  }
  if (!result.isNull(0, 5)) {
    std::int64_t expires = 0;
    if (!parseInt64(result.value(0, 5), expires)) {
      error = "invalid expires_at from PostgreSQL";
      return std::nullopt;
    }
    record.expires_epoch = expires;
  }
  return record;
}

RepoResult mapFailure(const PgResult& result, Metrics& metrics, Logger& logger) {
  const PgErrorInfo error = result.error();
  metrics.incDbError();
  logger.error("db_query_failed", {{"sqlstate", error.sqlstate}, {"detail", error.message}});

  if (error.isUniqueViolation() && result.constraintName() == "urls_short_code_key") {
    return RepoResult{RepoStatus::Collision, {}, error.message};
  }
  if (error.isConnectionError() || error.isQueryCanceled()) {
    return RepoResult{RepoStatus::Unavailable, {}, error.message};
  }
  return RepoResult{RepoStatus::Internal, {}, error.message};
}

}  // namespace

PostgresUrlRepository::PostgresUrlRepository(PgPool& pool, Metrics& metrics, Logger& logger,
                                             std::chrono::milliseconds acquire_timeout)
    : pool_(pool), metrics_(metrics), logger_(logger), acquire_timeout_(acquire_timeout) {}

RepoResult PostgresUrlRepository::create(const CreateUrlInput& input) {
  auto lease = pool_.acquire(acquire_timeout_);
  if (!lease) {
    return RepoResult{RepoStatus::Unavailable, {}, "database pool acquisition timed out"};
  }

  std::vector<std::optional<std::string>> params;
  params.reserve(3);
  params.emplace_back(input.short_code);
  params.emplace_back(input.original_url);
  if (input.expires_epoch.has_value()) {
    params.emplace_back(std::to_string(*input.expires_epoch));
  } else {
    params.emplace_back(std::nullopt);
  }

  DbQueryTimer timer(metrics_);
  auto result = (*lease)->execParams(kCreateSql, params);
  if (!result.ok()) {
    return mapFailure(result, metrics_, logger_);
  }

  std::string error;
  auto record = parseRecord(result, error);
  if (!record) {
    metrics_.incDbError();
    logger_.error("db_result_parse_failed", {{"detail", error}});
    return RepoResult{RepoStatus::Internal, {}, error};
  }
  return RepoResult{RepoStatus::Ok, std::move(*record), {}};
}

RepoResult PostgresUrlRepository::findByCode(const std::string& short_code) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto lease = pool_.acquire(acquire_timeout_);
    if (!lease) {
      return RepoResult{RepoStatus::Unavailable, {}, "database pool acquisition timed out"};
    }

    DbQueryTimer timer(metrics_);
    auto result = (*lease)->execParams(kFindSql, {short_code});
    if (!result.ok()) {
      const PgErrorInfo error = result.error();
      if (attempt == 0 && (error.isConnectionError() || error.isQueryCanceled())) {
        lease->markBroken();
        logger_.warn("db_read_retry", {{"sqlstate", error.sqlstate}});
        continue;
      }
      return mapFailure(result, metrics_, logger_);
    }

    if (result.rows() == 0) {
      return RepoResult{RepoStatus::NotFound, {}, {}};
    }

    std::string parse_error;
    auto record = parseRecord(result, parse_error);
    if (!record) {
      metrics_.incDbError();
      logger_.error("db_result_parse_failed", {{"detail", parse_error}});
      return RepoResult{RepoStatus::Internal, {}, parse_error};
    }
    return RepoResult{RepoStatus::Ok, std::move(*record), {}};
  }
  return RepoResult{RepoStatus::Unavailable, {}, "database read failed after retry"};
}

RepoResult PostgresUrlRepository::deactivate(const std::string& short_code) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto lease = pool_.acquire(acquire_timeout_);
    if (!lease) {
      return RepoResult{RepoStatus::Unavailable, {}, "database pool acquisition timed out"};
    }

    DbQueryTimer timer(metrics_);
    auto result = (*lease)->execParams(kDeactivateSql, {short_code});
    if (!result.ok()) {
      const PgErrorInfo error = result.error();
      if (attempt == 0 && (error.isConnectionError() || error.isQueryCanceled())) {
        lease->markBroken();
        logger_.warn("db_write_retry", {{"sqlstate", error.sqlstate}});
        continue;
      }
      return mapFailure(result, metrics_, logger_);
    }

    std::int64_t found = 0;
    if (result.rows() != 1 || !parseInt64(result.value(0, 0), found)) {
      metrics_.incDbError();
      logger_.error("db_result_parse_failed", {{"detail", "deactivate returned unexpected shape"}});
      return RepoResult{RepoStatus::Internal, {}, "unexpected deactivate result"};
    }
    if (found == 0) {
      return RepoResult{RepoStatus::NotFound, {}, {}};
    }
    return RepoResult{RepoStatus::Ok, {}, {}};
  }
  return RepoResult{RepoStatus::Unavailable, {}, "database write failed after retry"};
}

bool PostgresUrlRepository::ping() { return pool_.ping(acquire_timeout_); }

}  // namespace urlshort
