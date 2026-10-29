#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace urlshort {

std::int64_t nowEpochSeconds();

// Formats seconds since the Unix epoch as an RFC 3339 UTC timestamp with
// second precision, for example 2026-10-24T12:00:00Z.
std::string formatRfc3339(std::int64_t epoch_seconds);

// Parses an RFC 3339 timestamp. A timezone designator is mandatory. Fractional
// seconds are accepted and truncated. Returns nullopt for malformed input.
std::optional<std::int64_t> parseRfc3339(std::string_view text);

// Wall-clock timestamp with millisecond precision for log lines.
std::string formatLogTimestamp();

}  // namespace urlshort
