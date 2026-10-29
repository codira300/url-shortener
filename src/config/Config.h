#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace urlshort {

class ConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct HttpConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{8080};
  int io_threads{4};
  int max_connections{10000};
  int max_header_bytes{16384};
  int max_body_bytes{8192};
  int read_timeout_ms{15000};
  int write_timeout_ms{15000};
};

struct PgConfig {
  std::string dsn{"postgresql://urlshortener:urlshortener_dev@127.0.0.1:5433/urlshortener"};
  int pool_size{16};
  int acquire_timeout_ms{250};
  int connect_timeout_seconds{3};
  int statement_timeout_ms{2000};
};

struct RedisConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{6380};
  int db{0};
  std::string password{};
  int pool_size{16};
  int connect_timeout_ms{200};
  int socket_timeout_ms{200};
  int pool_wait_timeout_ms{100};
  int circuit_failure_threshold{5};
  int circuit_cooldown_ms{2000};
};

struct CacheConfig {
  int ttl_seconds{300};
  int negative_ttl_seconds{10};
  int tombstone_ttl_seconds{300};
  int ttl_jitter_percent{10};
};

struct RateLimitConfig {
  int redirect_rps{2000};
  int redirect_burst{4000};
  int create_rps{20};
  int create_burst{40};
  int admin_rps{100};
  int admin_burst{200};
  bool fail_open_redirect{true};
};

struct WorkerConfig {
  int threads{16};
  int queue_capacity{4096};
};

struct LogConfig {
  std::string level{"info"};
  bool access{false};
};

struct Config {
  HttpConfig http;
  PgConfig pg;
  RedisConfig redis;
  CacheConfig cache;
  RateLimitConfig rate_limit;
  WorkerConfig worker;
  LogConfig log;
  std::string public_base_url{"http://localhost:8080"};
  int short_code_length{7};
  int short_code_max_retries{5};
  bool trust_proxy{false};
  std::string trusted_proxy_cidrs{"127.0.0.1/32,::1/128"};
  int readiness_refresh_ms{1000};
  int shutdown_grace_ms{5000};

  static Config fromEnvironment(const std::unordered_map<std::string, std::string>& env);
  static Config fromProcessEnvironment();
};

std::string redactDsn(const std::string& dsn);

}  // namespace urlshort
