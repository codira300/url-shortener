#pragma once

#include <cstddef>
#include <string_view>

namespace urlshort {

class ShortCode {
 public:
  static constexpr std::size_t kMaxLength = 16;

  static bool isValid(std::string_view code);
  static bool isReserved(std::string_view code);
};

}  // namespace urlshort
