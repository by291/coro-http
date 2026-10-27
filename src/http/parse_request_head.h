#pragma once

#include <cstddef>
#include <string_view>

#include "http/request.h"

namespace coro_http {

struct parse_limits {
  std::size_t max_request_line{8 * 1024};
  std::size_t max_header_bytes{32 * 1024};
  std::size_t max_headers{100};
  std::size_t max_body_bytes{1 * 1024 * 1024};
};

enum class head_status { need_more, complete, invalid };

struct parsed_head {
  head_status status{head_status::need_more};
  request head;
  std::size_t content_length{0};
  bool keep_alive{true};
  std::size_t consumed{0};

  // Set when status == invalid: the status code to answer with.
  int error_status{0};
};

// Parses a request head from the bytes buffered so far.
//
// picohttpparser does the byte-level tokenizing; everything protocol-policy
// related (limits, Host, Content-Length vs Transfer-Encoding, keep-alive) is
// decided here so the session stays a plain request/response loop.
[[nodiscard]] parsed_head parse_request_head(std::string_view data,
                                             const parse_limits& limits = {});

}  // namespace coro_http
