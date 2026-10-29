#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace urlshort {

struct CidrBlock {
  bool ipv4{false};
  std::array<std::uint8_t, 16> bytes{};
  int prefix{0};

  bool contains(std::string_view address) const;
};

class TrustedProxyConfig {
 public:
  static std::optional<TrustedProxyConfig> parse(bool enabled, std::string_view csv,
                                                 std::string& error);

  bool enabled() const { return enabled_; }
  bool trusts(std::string_view peer_ip) const;

 private:
  bool enabled_{false};
  std::vector<CidrBlock> blocks_;
};

class ClientIdentity {
 public:
  // Returns a normalized client IP. The forwarded header is only consulted when
  // the immediate peer is a configured trusted proxy.
  static std::string resolve(std::string_view peer_ip, std::string_view forwarded_for,
                             const TrustedProxyConfig& trusted);

  static std::optional<std::string> normalizeIp(std::string_view ip);
};

}  // namespace urlshort
