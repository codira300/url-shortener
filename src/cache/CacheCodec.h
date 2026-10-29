#pragma once

#include "domain/UrlRecord.h"

#include <string>
#include <string_view>

namespace urlshort {

enum class CacheValueKind { Positive, Negative, Tombstone, Invalid };

struct DecodedCacheValue {
  CacheValueKind kind{CacheValueKind::Invalid};
  UrlRecord record;
};

// Wire format:
//   P|<created_epoch>|<expires_epoch or 0>|<original_url>
//   N
//   D
// The URL is the final field, so it may itself contain '|'.
std::string encodePositiveCacheValue(const UrlRecord& record);
DecodedCacheValue decodeCacheValue(std::string_view raw);

}  // namespace urlshort
