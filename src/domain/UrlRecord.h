#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace urlshort {

struct UrlRecord {
  std::int64_t id{0};
  std::string short_code;
  std::string original_url;
  std::int64_t created_epoch{0};
  std::optional<std::int64_t> expires_epoch;
  bool is_active{true};

  bool isExpired(std::int64_t now) const {
    return expires_epoch.has_value() && *expires_epoch <= now;
  }
};

}  // namespace urlshort
