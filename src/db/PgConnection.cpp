#include "db/PgConnection.h"

#include <utility>

namespace urlshort {
namespace {

bool startsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

}  // namespace

bool PgErrorInfo::isConnectionError() const {
  return startsWith(sqlstate, "08") || sqlstate == "57P01" || sqlstate == "57P02" ||
         sqlstate == "57P03" || sqlstate == "53300" || sqlstate == "53400";
}

bool PgErrorInfo::isUniqueViolation() const { return sqlstate == "23505"; }

bool PgErrorInfo::isQueryCanceled() const { return sqlstate == "57014"; }

bool PgErrorInfo::isTransient() const {
  return isConnectionError() || isQueryCanceled() || sqlstate == "40001" || sqlstate == "40P01" ||
         sqlstate == "55P03";
}

PgResult::PgResult(PGresult* result) : result_(result) {}

PgResult::~PgResult() {
  if (result_ != nullptr) {
    PQclear(result_);
  }
}

PgResult::PgResult(PgResult&& other) noexcept : result_(std::exchange(other.result_, nullptr)) {}

PgResult& PgResult::operator=(PgResult&& other) noexcept {
  if (this != &other) {
    if (result_ != nullptr) {
      PQclear(result_);
    }
    result_ = std::exchange(other.result_, nullptr);
  }
  return *this;
}

bool PgResult::ok() const {
  if (result_ == nullptr) {
    return false;
  }
  const auto status = PQresultStatus(result_);
  return status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK;
}

int PgResult::rows() const { return result_ == nullptr ? 0 : PQntuples(result_); }

int PgResult::columns() const { return result_ == nullptr ? 0 : PQnfields(result_); }

bool PgResult::isNull(int row, int column) const {
  return result_ == nullptr || PQgetisnull(result_, row, column) != 0;
}

std::string_view PgResult::value(int row, int column) const {
  if (result_ == nullptr || isNull(row, column)) {
    return {};
  }
  return std::string_view(PQgetvalue(result_, row, column),
                          static_cast<std::size_t>(PQgetlength(result_, row, column)));
}

std::string PgResult::errorMessage() const {
  if (result_ == nullptr) {
    return "no result";
  }
  const char* message = PQresultErrorMessage(result_);
  return message == nullptr ? std::string{} : std::string(message);
}

std::string PgResult::sqlState() const {
  if (result_ == nullptr) {
    return {};
  }
  const char* state = PQresultErrorField(result_, PG_DIAG_SQLSTATE);
  return state == nullptr ? std::string{} : std::string(state);
}

std::string PgResult::constraintName() const {
  if (result_ == nullptr) {
    return {};
  }
  const char* name = PQresultErrorField(result_, PG_DIAG_CONSTRAINT_NAME);
  return name == nullptr ? std::string{} : std::string(name);
}

PgErrorInfo PgResult::error() const { return PgErrorInfo{sqlState(), errorMessage()}; }

PgConnection::PgConnection(PGconn* connection) : connection_(connection) {}

PgConnection::~PgConnection() {
  if (connection_ != nullptr) {
    PQfinish(connection_);
  }
}

PgConnection::PgConnection(PgConnection&& other) noexcept
    : connection_(std::exchange(other.connection_, nullptr)) {}

PgConnection& PgConnection::operator=(PgConnection&& other) noexcept {
  if (this != &other) {
    if (connection_ != nullptr) {
      PQfinish(connection_);
    }
    connection_ = std::exchange(other.connection_, nullptr);
  }
  return *this;
}

PgResult PgConnection::exec(std::string_view sql) {
  if (connection_ == nullptr) {
    return PgResult{};
  }
  return PgResult{PQexec(connection_, std::string(sql).c_str())};
}

PgResult PgConnection::execParams(std::string_view sql,
                                  const std::vector<std::optional<std::string>>& params) {
  if (connection_ == nullptr) {
    return PgResult{};
  }

  std::vector<const char*> values;
  values.reserve(params.size());
  for (const auto& param : params) {
    values.push_back(param.has_value() ? param->c_str() : nullptr);
  }

  return PgResult{PQexecParams(connection_, std::string(sql).c_str(),
                               static_cast<int>(values.size()), nullptr, values.data(), nullptr,
                               nullptr, 0)};
}

bool PgConnection::isHealthy() const {
  return connection_ != nullptr && PQstatus(connection_) == CONNECTION_OK;
}

bool PgConnection::isIdle() const {
  return connection_ != nullptr && PQtransactionStatus(connection_) == PQTRANS_IDLE;
}

void PgConnection::rollbackIfNeeded() {
  if (connection_ != nullptr && !isIdle()) {
    PgResult ignored{PQexec(connection_, "ROLLBACK")};
    (void)ignored;
  }
}

std::string PgConnection::errorMessage() const {
  if (connection_ == nullptr) {
    return "no connection";
  }
  const char* message = PQerrorMessage(connection_);
  return message == nullptr ? std::string{} : std::string(message);
}

}  // namespace urlshort
