#include "domain/UrlValidator.h"

#include <boost/url.hpp>
#include <cctype>

namespace urlshort {
namespace {

std::string lowerAscii(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (char c : input) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

}  // namespace

UrlValidationResult validateHttpUrl(std::string_view url) {
  if (url.empty()) {
    return UrlValidationResult::fail("url must not be empty");
  }
  if (url.size() > kMaxUrlLength) {
    return UrlValidationResult::fail("url exceeds the maximum length");
  }
  for (char c : url) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || uc == 0x7f) {
      return UrlValidationResult::fail("url must not contain control characters");
    }
  }

  auto parsed = boost::urls::parse_uri(url);
  if (!parsed) {
    return UrlValidationResult::fail("url is not a valid absolute URI");
  }
  const boost::urls::url_view view = *parsed;
  if (!view.has_scheme() || view.host().empty()) {
    return UrlValidationResult::fail("url must contain a scheme and a host");
  }

  const std::string scheme = lowerAscii(view.scheme());
  if (scheme != "http" && scheme != "https") {
    return UrlValidationResult::fail("only http and https URLs are supported");
  }
  if (view.host().empty()) {
    return UrlValidationResult::fail("url host must not be empty");
  }
  return UrlValidationResult::ok();
}

}  // namespace urlshort
