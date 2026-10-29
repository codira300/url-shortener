#pragma once

#include "db/UrlRepository.h"

#include <chrono>

namespace urlshort {

class PgPool;
class Metrics;
class Logger;

class PostgresUrlRepository final : public UrlRepository {
 public:
  PostgresUrlRepository(PgPool& pool, Metrics& metrics, Logger& logger,
                        std::chrono::milliseconds acquire_timeout);

  RepoResult create(const CreateUrlInput& input) override;
  RepoResult findByCode(const std::string& short_code) override;
  RepoResult deactivate(const std::string& short_code) override;
  bool ping() override;

 private:
  PgPool& pool_;
  Metrics& metrics_;
  Logger& logger_;
  std::chrono::milliseconds acquire_timeout_;
};

}  // namespace urlshort
