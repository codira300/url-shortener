#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace urlshort {

struct UrlValidationResult {
  bool valid{false};
  std::string reason;

  static UrlValidationResult ok() { return {true, {}}; }
  static UrlValidationResult fail(std::string why) { return {false, std::move(why)}; }
};

inline constexpr std::size_t kMaxUrlLength = 2048;

UrlValidationResult validateHttpUrl(std::string_view url);

}  // namespace urlshort
