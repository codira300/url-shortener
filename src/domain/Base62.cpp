#include "domain/Base62.h"

#include <algorithm>
#include <limits>

namespace urlshort::base62 {

std::string encode(std::uint64_t value) {
  if (value == 0) {
    return "0";
  }
  std::string out;
  out.reserve(11);
  while (value > 0) {
    out.push_back(kAlphabet[value % kAlphabet.size()]);
    value /= kAlphabet.size();
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::optional<std::uint64_t> decode(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (char c : text) {
    const auto position = kAlphabet.find(c);
    if (position == std::string_view::npos) {
      return std::nullopt;
    }
    if (value > (std::numeric_limits<std::uint64_t>::max() - position) / kAlphabet.size()) {
      return std::nullopt;
    }
    value = value * kAlphabet.size() + position;
  }
  return value;
}

}  // namespace urlshort::base62
