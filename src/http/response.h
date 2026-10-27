#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "http/request.h"

namespace coro_http {

struct response {
  int status{200};
  std::vector<header_field> headers;
  std::string body;

  // Set by a handler or the session to end the connection regardless of the
  // protocol's default.
  bool close{false};

  static response with_text(std::string body, int status = 200);
  static response with_status(int status);

  [[nodiscard]] std::string_view header(std::string_view name) const;
  void set_header(std::string name, std::string value);
};

[[nodiscard]] std::string status_text(int status);

// Content-Length and Connection are appended here so handlers never have to
// think about framing. A HEAD response keeps its Content-Length but no body.
[[nodiscard]] std::string serialize_response(const response& res, bool keep_alive, bool head_request);

}  // namespace coro_http
