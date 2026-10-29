#pragma once

#include <cstddef>
#include <string>

namespace urlshort {

class ShortCodeGenerator {
 public:
  virtual ~ShortCodeGenerator() = default;
  virtual std::string generate(std::size_t length) = 0;
};

// Generates cryptographically strong, uniformly distributed base62 codes using
// the operating system CSPRNG exposed by OpenSSL RAND_bytes. Rejection sampling
// avoids the modulo bias that a naive byte % 62 would introduce.
class RandomShortCodeGenerator final : public ShortCodeGenerator {
 public:
  std::string generate(std::size_t length) override;
};

}  // namespace urlshort
