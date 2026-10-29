#pragma once

#include "domain/UrlRecord.h"

#include <cstdint>
#include <optional>
#include <string>

namespace urlshort {

enum class RepoStatus {
  Ok,
  NotFound,
  Collision,
  Unavailable,
  Internal,
};

struct RepoResult {
  RepoStatus status{RepoStatus::Internal};
  UrlRecord record;
  std::string detail;
};

struct CreateUrlInput {
  std::string short_code;
  std::string original_url;
  std::optional<std::int64_t> expires_epoch;
};

class UrlRepository {
 public:
  virtual ~UrlRepository() = default;
  virtual RepoResult create(const CreateUrlInput& input) = 0;
  virtual RepoResult findByCode(const std::string& short_code) = 0;
  // Soft-deletes the row. Returns Ok when the code exists (whether it was
  // already inactive), NotFound otherwise.
  virtual RepoResult deactivate(const std::string& short_code) = 0;
  virtual bool ping() = 0;
};

}  // namespace urlshort
