#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace urlshort::base62 {

inline constexpr std::string_view kAlphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

std::string encode(std::uint64_t value);

// Returns nullopt when the input is empty, contains a character outside the
// alphabet, or overflows a 64-bit unsigned integer.
std::optional<std::uint64_t> decode(std::string_view text);

}  // namespace urlshort::base62
