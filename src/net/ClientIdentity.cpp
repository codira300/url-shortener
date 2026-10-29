#include "net/ClientIdentity.h"

#include <boost/asio/ip/address.hpp>
#include <charconv>
#include <cctype>
#include <sstream>

namespace urlshort {
namespace {

std::string_view trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return value;
}

bool matchesPrefix(const std::array<std::uint8_t, 16>& left,
                   const std::array<std::uint8_t, 16>& right, int prefix) {
  const int full_bytes = prefix / 8;
  const int remainder = prefix % 8;
  for (int i = 0; i < full_bytes; ++i) {
    if (left[static_cast<std::size_t>(i)] != right[static_cast<std::size_t>(i)]) {
      return false;
    }
  }
  if (remainder == 0) {
    return true;
  }
  const std::uint8_t mask = static_cast<std::uint8_t>(0xFFu << (8 - remainder));
  return (left[static_cast<std::size_t>(full_bytes)] & mask) ==
         (right[static_cast<std::size_t>(full_bytes)] & mask);
}

std::array<std::uint8_t, 16> toBytes(const boost::asio::ip::address& address) {
  std::array<std::uint8_t, 16> bytes{};
  if (address.is_v4()) {
    const auto v4 = address.to_v4().to_bytes();
    for (std::size_t i = 0; i < 4; ++i) {
      bytes[i] = v4[i];
    }
  } else {
    const auto v6 = address.to_v6().to_bytes();
    for (std::size_t i = 0; i < 16; ++i) {
      bytes[i] = v6[i];
    }
  }
  return bytes;
}

}  // namespace

bool CidrBlock::contains(std::string_view address) const {
  boost::system::error_code ec;
  const auto parsed = boost::asio::ip::make_address(std::string(address), ec);
  if (ec) {
    return false;
  }
  if (parsed.is_v4() != ipv4) {
    return false;
  }
  return matchesPrefix(bytes, toBytes(parsed), prefix);
}

std::optional<TrustedProxyConfig> TrustedProxyConfig::parse(bool enabled, std::string_view csv,
                                                            std::string& error) {
  TrustedProxyConfig config;
  config.enabled_ = enabled;
  if (!enabled) {
    return config;
  }

  std::size_t start = 0;
  while (start <= csv.size()) {
    const std::size_t comma = csv.find(',', start);
    const std::string_view token =
        trim(csv.substr(start, comma == std::string_view::npos ? csv.size() - start : comma - start));
    if (!token.empty()) {
      const std::size_t slash = token.find('/');
      const std::string_view address_part = slash == std::string_view::npos ? token : token.substr(0, slash);
      boost::system::error_code ec;
      const auto address = boost::asio::ip::make_address(std::string(address_part), ec);
      if (ec) {
        error = "invalid trusted proxy CIDR address: " + std::string(address_part);
        return std::nullopt;
      }

      int prefix = address.is_v4() ? 32 : 128;
      if (slash != std::string_view::npos) {
        const std::string_view prefix_part = token.substr(slash + 1);
        const char* begin = prefix_part.data();
        const char* end = prefix_part.data() + prefix_part.size();
        auto result = std::from_chars(begin, end, prefix);
        if (result.ec != std::errc{} || result.ptr != end) {
          error = "invalid trusted proxy CIDR prefix: " + std::string(token);
          return std::nullopt;
        }
      }
      const int max_prefix = address.is_v4() ? 32 : 128;
      if (prefix < 0 || prefix > max_prefix) {
        error = "trusted proxy CIDR prefix out of range: " + std::string(token);
        return std::nullopt;
      }

      CidrBlock block;
      block.ipv4 = address.is_v4();
      block.bytes = toBytes(address);
      block.prefix = prefix;
      config.blocks_.push_back(block);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }

  return config;
}

bool TrustedProxyConfig::trusts(std::string_view peer_ip) const {
  if (!enabled_) {
    return false;
  }
  for (const auto& block : blocks_) {
    if (block.contains(peer_ip)) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> ClientIdentity::normalizeIp(std::string_view ip) {
  boost::system::error_code ec;
  const auto parsed = boost::asio::ip::make_address(std::string(trim(ip)), ec);
  if (ec) {
    return std::nullopt;
  }
  return parsed.to_string();
}

std::string ClientIdentity::resolve(std::string_view peer_ip, std::string_view forwarded_for,
                                    const TrustedProxyConfig& trusted) {
  const std::string peer = normalizeIp(peer_ip).value_or(std::string(peer_ip));
  if (!trusted.enabled() || !trusted.trusts(peer) || forwarded_for.empty()) {
    return peer;
  }

  std::size_t start = 0;
  while (start <= forwarded_for.size()) {
    const std::size_t comma = forwarded_for.find(',', start);
    const std::string_view token = trim(forwarded_for.substr(
        start, comma == std::string_view::npos ? forwarded_for.size() - start : comma - start));
    if (!token.empty()) {
      if (auto normalized = normalizeIp(token)) {
        return *normalized;
      }
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return peer;
}

}  // namespace urlshort
