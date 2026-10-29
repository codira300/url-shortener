#include "domain/ShortCodeGenerator.h"

#include "domain/Base62.h"

#include <array>
#include <openssl/rand.h>
#include <stdexcept>

namespace urlshort {

std::string RandomShortCodeGenerator::generate(std::size_t length) {
  if (length == 0) {
    throw std::invalid_argument("short code length must be positive");
  }

  std::string out;
  out.reserve(length);

  constexpr std::size_t kBatch = 64;
  constexpr unsigned kRejectionLimit =
      static_cast<unsigned>(256 - (256 % base62::kAlphabet.size()));  // 248
  std::array<unsigned char, kBatch> buffer{};

  while (out.size() < length) {
    if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
      throw std::runtime_error("CSPRNG failure while generating a short code");
    }
    for (unsigned char byte : buffer) {
      if (byte >= kRejectionLimit) {
        continue;
      }
      out.push_back(base62::kAlphabet[byte % base62::kAlphabet.size()]);
      if (out.size() == length) {
        break;
      }
    }
  }
  return out;
}

}  // namespace urlshort
