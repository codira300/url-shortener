#pragma once

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace urlshort {

struct HttpRequest {
  std::string method;
  std::string target;
  std::string peer_ip;
  std::string forwarded_for;
  std::string body;
  bool keep_alive{false};
  std::chrono::steady_clock::time_point received_at{};
};

struct HttpResponse {
  int status{200};
  std::string content_type{"application/json"};
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  bool keep_alive{true};

  void addHeader(std::string name, std::string value) {
    headers.emplace_back(std::move(name), std::move(value));
  }
};

}  // namespace urlshort
