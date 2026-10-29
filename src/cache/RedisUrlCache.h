#pragma once

#include "config/Config.h"
#include "domain/UrlRecord.h"

#include <optional>
#include <string>

namespace urlshort {

class RedisGateway;
class Metrics;
class Logger;

enum class CacheStatus { Hit, Negative, Miss, Unavailable };

struct CacheLookupResult {
  CacheStatus status{CacheStatus::Miss};
  std::optional<UrlRecord> record;
};

class UrlCache {
 public:
  virtual ~UrlCache() = default;
  virtual CacheLookupResult lookup(const std::string& short_code) = 0;
  // Cache-aside population after a database read. Refuses to overwrite a
  // deletion tombstone.
  virtual void populate(const UrlRecord& record) = 0;
  // Authoritative population after a successful INSERT. The code is new, so no
  // tombstone can exist for it.
  virtual void storeCreated(const UrlRecord& record) = 0;
  virtual void storeNegative(const std::string& short_code) = 0;
  // Writes a tombstone instead of deleting the key, closing the read-then-delete
  // race that could otherwise resurrect a stale positive entry. Returns false
  // only when the tombstone could not be written; callers must not report
  // successful revocation in that case.
  virtual bool invalidate(const std::string& short_code) = 0;
};

class RedisUrlCache final : public UrlCache {
 public:
  RedisUrlCache(RedisGateway& redis, const CacheConfig& config, Metrics& metrics, Logger& logger);

  CacheLookupResult lookup(const std::string& short_code) override;
  void populate(const UrlRecord& record) override;
  void storeCreated(const UrlRecord& record) override;
  void storeNegative(const std::string& short_code) override;
  bool invalidate(const std::string& short_code) override;

  static std::string cacheKey(const std::string& short_code);

 private:
  int positiveTtlMs(const UrlRecord& record, std::int64_t now) const;

  RedisGateway& redis_;
  CacheConfig config_;
  Metrics& metrics_;
  Logger& logger_;
};

}  // namespace urlshort
