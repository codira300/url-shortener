#include "cache/CacheCodec.h"

#include <charconv>
#include <optional>

namespace urlshort {
namespace {

std::optional<std::int64_t> parseInt64(std::string_view text) {
  std::int64_t value = 0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

std::string encodePositiveCacheValue(const UrlRecord& record) {
  std::string out;
  out.reserve(record.original_url.size() + 32);
  out.append("P|");
  out.append(std::to_string(record.created_epoch));
  out.push_back('|');
  out.append(std::to_string(record.expires_epoch.value_or(0)));
  out.push_back('|');
  out.append(record.original_url);
  return out;
}

DecodedCacheValue decodeCacheValue(std::string_view raw) {
  if (raw == "N") {
    DecodedCacheValue decoded;
    decoded.kind = CacheValueKind::Negative;
    return decoded;
  }
  if (raw == "D") {
    DecodedCacheValue decoded;
    decoded.kind = CacheValueKind::Tombstone;
    return decoded;
  }
  if (raw.size() < 4 || raw[0] != 'P' || raw[1] != '|') {
    return DecodedCacheValue{};
  }

  const std::size_t first = raw.find('|', 2);
  if (first == std::string_view::npos) {
    return DecodedCacheValue{};
  }
  const std::size_t second = raw.find('|', first + 1);
  if (second == std::string_view::npos) {
    return DecodedCacheValue{};
  }

  const auto created = parseInt64(raw.substr(2, first - 2));
  const auto expires = parseInt64(raw.substr(first + 1, second - first - 1));
  if (!created || !expires || *expires < 0) {
    return DecodedCacheValue{};
  }

  DecodedCacheValue decoded;
  decoded.kind = CacheValueKind::Positive;
  decoded.record.created_epoch = *created;
  if (*expires > 0) {
    decoded.record.expires_epoch = *expires;
  }
  decoded.record.original_url = std::string(raw.substr(second + 1));
  decoded.record.is_active = true;
  if (decoded.record.original_url.empty()) {
    return DecodedCacheValue{};
  }
  return decoded;
}

}  // namespace urlshort
