#include "config/Config.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cctype>
#include <sstream>
#include <string_view>
#include <vector>

namespace urlshort {
namespace {

struct Reader {
  const std::unordered_map<std::string, std::string>& env;
  std::vector<std::string>& errors;

  std::optional<std::string> raw(std::string_view key) const {
    auto it = env.find(std::string(key));
    if (it == env.end() || it->second.empty()) {
      return std::nullopt;
    }
    return it->second;
  }

  std::string str(std::string_view key, std::string fallback) const {
    if (auto value = raw(key)) {
      return *value;
    }
    return fallback;
  }

  int integer(std::string_view key, int fallback, int min_value, int max_value) const {
    auto value = raw(key);
    if (!value) {
      return fallback;
    }
    int parsed = 0;
    const char* begin = value->data();
    const char* end = value->data() + value->size();
    auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
      errors.push_back("environment variable " + std::string(key) + " is not a valid integer");
      return fallback;
    }
    if (parsed < min_value || parsed > max_value) {
      errors.push_back("environment variable " + std::string(key) + " must be between " +
                       std::to_string(min_value) + " and " + std::to_string(max_value));
      return fallback;
    }
    return parsed;
  }

  bool boolean(std::string_view key, bool fallback) const {
    auto value = raw(key);
    if (!value) {
      return fallback;
    }
    std::string lowered;
    lowered.reserve(value->size());
    for (char c : *value) {
      lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
      return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
      return false;
    }
    errors.push_back("environment variable " + std::string(key) + " is not a boolean");
    return fallback;
  }
};

bool validBaseUrl(const std::string& value) {
  if (value.empty()) {
    return false;
  }
  const bool http = value.rfind("http://", 0) == 0;
  const bool https = value.rfind("https://", 0) == 0;
  if (!http && !https) {
    return false;
  }
  if (value.back() == '/') {
    return false;
  }
  for (char c : value) {
    if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) == 0x7f) {
      return false;
    }
  }
  return true;
}

bool validLogLevel(const std::string& value) {
  return value == "debug" || value == "info" || value == "warn" || value == "error";
}

}  // namespace

Config Config::fromEnvironment(const std::unordered_map<std::string, std::string>& env) {
  std::vector<std::string> errors;
  Reader reader{env, errors};
  Config cfg;

  cfg.http.host = reader.str("HTTP_HOST", cfg.http.host);
  cfg.http.port = static_cast<std::uint16_t>(reader.integer("HTTP_PORT", cfg.http.port, 0, 65535));
  cfg.http.io_threads = reader.integer("HTTP_IO_THREADS", cfg.http.io_threads, 1, 128);
  cfg.http.max_connections =
      reader.integer("HTTP_MAX_CONNECTIONS", cfg.http.max_connections, 1, 1000000);
  cfg.http.max_header_bytes =
      reader.integer("HTTP_MAX_HEADER_BYTES", cfg.http.max_header_bytes, 1024, 1024 * 1024);
  cfg.http.max_body_bytes =
      reader.integer("HTTP_MAX_BODY_BYTES", cfg.http.max_body_bytes, 128, 1024 * 1024);
  cfg.http.read_timeout_ms =
      reader.integer("HTTP_READ_TIMEOUT_MS", cfg.http.read_timeout_ms, 100, 300000);
  cfg.http.write_timeout_ms =
      reader.integer("HTTP_WRITE_TIMEOUT_MS", cfg.http.write_timeout_ms, 100, 300000);

  cfg.pg.dsn = reader.str("PG_DSN", cfg.pg.dsn);
  cfg.pg.pool_size = reader.integer("PG_POOL_SIZE", cfg.pg.pool_size, 1, 256);
  cfg.pg.acquire_timeout_ms =
      reader.integer("PG_ACQUIRE_TIMEOUT_MS", cfg.pg.acquire_timeout_ms, 1, 60000);
  cfg.pg.connect_timeout_seconds =
      reader.integer("PG_CONNECT_TIMEOUT_SECONDS", cfg.pg.connect_timeout_seconds, 1, 60);
  cfg.pg.statement_timeout_ms =
      reader.integer("PG_STATEMENT_TIMEOUT_MS", cfg.pg.statement_timeout_ms, 1, 60000);

  cfg.redis.host = reader.str("REDIS_HOST", cfg.redis.host);
  cfg.redis.port = static_cast<std::uint16_t>(reader.integer("REDIS_PORT", cfg.redis.port, 1, 65535));
  cfg.redis.db = reader.integer("REDIS_DB", cfg.redis.db, 0, 15);
  cfg.redis.password = reader.str("REDIS_PASSWORD", cfg.redis.password);
  cfg.redis.pool_size = reader.integer("REDIS_POOL_SIZE", cfg.redis.pool_size, 1, 256);
  cfg.redis.connect_timeout_ms =
      reader.integer("REDIS_CONNECT_TIMEOUT_MS", cfg.redis.connect_timeout_ms, 10, 60000);
  cfg.redis.socket_timeout_ms =
      reader.integer("REDIS_SOCKET_TIMEOUT_MS", cfg.redis.socket_timeout_ms, 10, 60000);
  cfg.redis.pool_wait_timeout_ms =
      reader.integer("REDIS_POOL_WAIT_TIMEOUT_MS", cfg.redis.pool_wait_timeout_ms, 1, 60000);
  cfg.redis.circuit_failure_threshold = reader.integer(
      "REDIS_CIRCUIT_FAILURE_THRESHOLD", cfg.redis.circuit_failure_threshold, 1, 1000);
  cfg.redis.circuit_cooldown_ms =
      reader.integer("REDIS_CIRCUIT_COOLDOWN_MS", cfg.redis.circuit_cooldown_ms, 100, 600000);

  cfg.cache.ttl_seconds = reader.integer("CACHE_TTL_SECONDS", cfg.cache.ttl_seconds, 1, 86400);
  cfg.cache.negative_ttl_seconds =
      reader.integer("CACHE_NEGATIVE_TTL_SECONDS", cfg.cache.negative_ttl_seconds, 1, 3600);
  cfg.cache.tombstone_ttl_seconds =
      reader.integer("CACHE_TOMBSTONE_TTL_SECONDS", cfg.cache.tombstone_ttl_seconds, 1, 86400);
  cfg.cache.ttl_jitter_percent =
      reader.integer("CACHE_TTL_JITTER_PERCENT", cfg.cache.ttl_jitter_percent, 0, 50);

  cfg.rate_limit.redirect_rps =
      reader.integer("REDIRECT_RATE_LIMIT_RPS", cfg.rate_limit.redirect_rps, 1, 1000000);
  cfg.rate_limit.redirect_burst =
      reader.integer("REDIRECT_RATE_LIMIT_BURST", cfg.rate_limit.redirect_burst, 1, 2000000);
  cfg.rate_limit.create_rps =
      reader.integer("CREATE_RATE_LIMIT_RPS", cfg.rate_limit.create_rps, 1, 1000000);
  cfg.rate_limit.create_burst =
      reader.integer("CREATE_RATE_LIMIT_BURST", cfg.rate_limit.create_burst, 1, 2000000);
  cfg.rate_limit.admin_rps =
      reader.integer("ADMIN_RATE_LIMIT_RPS", cfg.rate_limit.admin_rps, 1, 1000000);
  cfg.rate_limit.admin_burst =
      reader.integer("ADMIN_RATE_LIMIT_BURST", cfg.rate_limit.admin_burst, 1, 2000000);
  cfg.rate_limit.fail_open_redirect =
      reader.boolean("RATE_LIMIT_FAIL_OPEN_REDIRECT", cfg.rate_limit.fail_open_redirect);

  cfg.worker.threads = reader.integer("WORKER_THREADS", cfg.worker.threads, 1, 256);
  cfg.worker.queue_capacity =
      reader.integer("WORKER_QUEUE_CAPACITY", cfg.worker.queue_capacity, 1, 1000000);

  cfg.log.level = reader.str("LOG_LEVEL", cfg.log.level);
  cfg.log.access = reader.boolean("LOG_ACCESS", cfg.log.access);

  cfg.public_base_url = reader.str("PUBLIC_BASE_URL", cfg.public_base_url);
  cfg.short_code_length = reader.integer("SHORT_CODE_LENGTH", cfg.short_code_length, 4, 16);
  cfg.short_code_max_retries =
      reader.integer("SHORT_CODE_MAX_RETRIES", cfg.short_code_max_retries, 1, 20);
  cfg.trust_proxy = reader.boolean("TRUST_PROXY", cfg.trust_proxy);
  cfg.trusted_proxy_cidrs = reader.str("TRUSTED_PROXY_CIDRS", cfg.trusted_proxy_cidrs);
  cfg.readiness_refresh_ms =
      reader.integer("READINESS_REFRESH_MS", cfg.readiness_refresh_ms, 100, 60000);
  cfg.shutdown_grace_ms = reader.integer("SHUTDOWN_GRACE_MS", cfg.shutdown_grace_ms, 0, 300000);

  if (cfg.http.host.empty()) {
    errors.emplace_back("HTTP_HOST must not be empty");
  }
  if (cfg.pg.dsn.empty()) {
    errors.emplace_back("PG_DSN must not be empty");
  }
  if (cfg.redis.host.empty()) {
    errors.emplace_back("REDIS_HOST must not be empty");
  }
  if (!validBaseUrl(cfg.public_base_url)) {
    errors.emplace_back("PUBLIC_BASE_URL must be an absolute http(s) URL without a trailing slash");
  }
  if (!validLogLevel(cfg.log.level)) {
    errors.emplace_back("LOG_LEVEL must be one of debug, info, warn, error");
  }
  if (cfg.cache.tombstone_ttl_seconds < cfg.cache.negative_ttl_seconds) {
    errors.emplace_back("CACHE_TOMBSTONE_TTL_SECONDS must be >= CACHE_NEGATIVE_TTL_SECONDS");
  }

  if (!errors.empty()) {
    std::ostringstream out;
    out << "invalid configuration:";
    for (const auto& error : errors) {
      out << " [" << error << "]";
    }
    throw ConfigError(out.str());
  }

  return cfg;
}

Config Config::fromProcessEnvironment() {
  std::unordered_map<std::string, std::string> env;
  // Only read the variables this application understands. This keeps unrelated
  // process state out of the configuration object and makes tests deterministic.
  static const char* keys[] = {
      "HTTP_HOST",          "HTTP_PORT",          "HTTP_IO_THREADS",
      "HTTP_MAX_CONNECTIONS", "HTTP_MAX_HEADER_BYTES", "HTTP_MAX_BODY_BYTES",
      "HTTP_READ_TIMEOUT_MS", "HTTP_WRITE_TIMEOUT_MS", "PG_DSN",
      "PG_POOL_SIZE",       "PG_ACQUIRE_TIMEOUT_MS", "PG_CONNECT_TIMEOUT_SECONDS",
      "PG_STATEMENT_TIMEOUT_MS", "REDIS_HOST",    "REDIS_PORT",
      "REDIS_DB",           "REDIS_PASSWORD",     "REDIS_POOL_SIZE",
      "REDIS_CONNECT_TIMEOUT_MS", "REDIS_SOCKET_TIMEOUT_MS", "REDIS_POOL_WAIT_TIMEOUT_MS",
      "REDIS_CIRCUIT_FAILURE_THRESHOLD", "REDIS_CIRCUIT_COOLDOWN_MS",
      "CACHE_TTL_SECONDS",  "CACHE_NEGATIVE_TTL_SECONDS", "CACHE_TOMBSTONE_TTL_SECONDS",
      "CACHE_TTL_JITTER_PERCENT", "REDIRECT_RATE_LIMIT_RPS", "REDIRECT_RATE_LIMIT_BURST",
      "CREATE_RATE_LIMIT_RPS", "CREATE_RATE_LIMIT_BURST", "ADMIN_RATE_LIMIT_RPS",
      "ADMIN_RATE_LIMIT_BURST", "RATE_LIMIT_FAIL_OPEN_REDIRECT", "WORKER_THREADS",
      "WORKER_QUEUE_CAPACITY", "LOG_LEVEL",        "LOG_ACCESS",
      "PUBLIC_BASE_URL",    "SHORT_CODE_LENGTH",   "SHORT_CODE_MAX_RETRIES",
      "TRUST_PROXY",        "TRUSTED_PROXY_CIDRS", "READINESS_REFRESH_MS",
      "SHUTDOWN_GRACE_MS",
  };
  for (const char* key : keys) {
    if (const char* value = std::getenv(key); value != nullptr) {
      env.emplace(key, value);
    }
  }
  return fromEnvironment(env);
}

std::string redactDsn(const std::string& dsn) {
  std::string out = dsn;
  const auto scheme = out.find("://");
  if (scheme != std::string::npos) {
    const auto at = out.find('@', scheme + 3);
    const auto colon = out.find(':', scheme + 3);
    if (at != std::string::npos && colon != std::string::npos && colon < at) {
      out.replace(colon + 1, at - colon - 1, "***");
    }
  }
  const std::string key = "password=";
  auto pos = out.find(key);
  while (pos != std::string::npos) {
    const auto value_start = pos + key.size();
    const auto value_end = out.find_first_of(" ;", value_start);
    const auto count = (value_end == std::string::npos ? out.size() : value_end) - value_start;
    out.replace(value_start, count, "***");
    pos = out.find(key, value_start + 3);
  }
  return out;
}

}  // namespace urlshort
