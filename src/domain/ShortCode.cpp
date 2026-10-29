#include "domain/ShortCode.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace urlshort {
namespace {

bool isAlphaNumeric(char c) {
  const unsigned char uc = static_cast<unsigned char>(c);
  return (uc >= '0' && uc <= '9') || (uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z');
}

std::string_view lowerCopy(std::string_view input, std::array<char, ShortCode::kMaxLength + 1>& storage) {
  const std::size_t count = std::min(input.size(), ShortCode::kMaxLength);
  for (std::size_t i = 0; i < count; ++i) {
    storage[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(input[i])));
  }
  return std::string_view(storage.data(), count);
}

}  // namespace

bool ShortCode::isValid(std::string_view code) {
  if (code.empty() || code.size() > kMaxLength) {
    return false;
  }
  return std::all_of(code.begin(), code.end(), isAlphaNumeric);
}

bool ShortCode::isReserved(std::string_view code) {
  static constexpr std::array<std::string_view, 15> kReserved = {
      "api",   "health", "metrics", "static", "admin",  "login", "signup", "about",
      "terms", "privacy", "favicon.ico", "robots.txt", "index.html", "www", "assets",
  };
  if (code.empty() || code.size() > kMaxLength) {
    return false;
  }
  std::array<char, kMaxLength + 1> storage{};
  const std::string_view lowered = lowerCopy(code, storage);
  return std::find(kReserved.begin(), kReserved.end(), lowered) != kReserved.end();
}

}  // namespace urlshort
