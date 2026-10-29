#pragma once

#include <libpq-fe.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace urlshort {

struct PgErrorInfo {
  std::string sqlstate;
  std::string message;

  bool isConnectionError() const;
  bool isUniqueViolation() const;
  bool isQueryCanceled() const;
  bool isTransient() const;
};

class PgResult {
 public:
  PgResult() = default;
  explicit PgResult(PGresult* result);
  ~PgResult();

  PgResult(const PgResult&) = delete;
  PgResult& operator=(const PgResult&) = delete;
  PgResult(PgResult&& other) noexcept;
  PgResult& operator=(PgResult&& other) noexcept;

  bool ok() const;
  int rows() const;
  int columns() const;
  bool isNull(int row, int column) const;
  std::string_view value(int row, int column) const;
  std::string errorMessage() const;
  std::string sqlState() const;
  std::string constraintName() const;
  PgErrorInfo error() const;

 private:
  PGresult* result_{nullptr};
};

class PgConnection {
 public:
  explicit PgConnection(PGconn* connection);
  ~PgConnection();

  PgConnection(const PgConnection&) = delete;
  PgConnection& operator=(const PgConnection&) = delete;
  PgConnection(PgConnection&& other) noexcept;
  PgConnection& operator=(PgConnection&& other) noexcept;

  PgResult exec(std::string_view sql);
  PgResult execParams(std::string_view sql, const std::vector<std::optional<std::string>>& params);

  bool isHealthy() const;
  bool isIdle() const;
  void rollbackIfNeeded();
  std::string errorMessage() const;
  PGconn* raw() const { return connection_; }

 private:
  PGconn* connection_{nullptr};
};

}  // namespace urlshort
