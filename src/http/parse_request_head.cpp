#include "http/parse_request_head.h"

#include <ada.h>
#include <picohttpparser.h>

#include <limits>
#include <string>

namespace coro_http {

namespace {

// picohttpparser needs a fixed header array. Anything above parse_limits'
// default only costs stack space here; the actual policy limit is checked
// after the parse so the client gets a precise 431.
constexpr std::size_t kHeaderArraySize = 256;

parsed_head invalid(int status) {
  parsed_head result;
  result.status = head_status::invalid;
  result.error_status = status;
  return result;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

// Splits a comma-separated header value and compares tokens case-insensitively.
bool has_token(std::string_view value, std::string_view token) {
  while (!value.empty()) {
    auto comma = value.find(',');
    std::string_view part = value.substr(0, comma);
    if (detail::ascii_iequals(trim(part), token)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    value.remove_prefix(comma + 1);
  }
  return false;
}

bool parse_content_length(std::string_view text, std::size_t& out) {
  text = trim(text);
  if (text.empty()) {
    return false;
  }
  std::size_t value = 0;
  for (char digit : text) {
    if (digit < '0' || digit > '9') {
      return false;
    }
    auto next = static_cast<std::size_t>(digit - '0');
    if (value > (std::numeric_limits<std::size_t>::max() - next) / 10) {
      return false;  // overflow
    }
    value = value * 10 + next;
  }
  out = value;
  return true;
}

}  // namespace

parsed_head parse_request_head(std::string_view data, const parse_limits& limits) {
  // The request line ends at the first LF (picohttpparser accepts bare LF as
  // well as CRLF). Checking it before the full parse keeps a slow drip of one
  // very long line from growing the buffer forever.
  auto newline = data.find('\n');
  std::size_t line_length = newline == std::string_view::npos ? data.size() : newline;
  if (line_length > limits.max_request_line) {
    return invalid(414);
  }

  // picohttpparser only understands HTTP/1.x; recognise other textual versions
  // here so a client gets 505 instead of a generic 400.
  if (newline != std::string_view::npos) {
    std::string_view line = data.substr(0, newline);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    auto last_space = line.rfind(' ');
    if (last_space != std::string_view::npos) {
      std::string_view version = line.substr(last_space + 1);
      if (version.starts_with("HTTP/") && version != "HTTP/1.0" && version != "HTTP/1.1") {
        return invalid(505);
      }
    }
  }

  phr_header headers[kHeaderArraySize];
  std::size_t num_headers = kHeaderArraySize;
  const char* method = nullptr;
  std::size_t method_len = 0;
  const char* target = nullptr;
  std::size_t target_len = 0;
  int minor_version = -1;

  int parsed = phr_parse_request(data.data(), data.size(), &method, &method_len, &target,
                                 &target_len, &minor_version, headers, &num_headers, 0);

  if (parsed == -2) {
    if (data.size() > limits.max_header_bytes) {
      return invalid(431);
    }
    parsed_head result;
    result.status = head_status::need_more;
    return result;
  }
  if (parsed < 0) {
    return invalid(400);
  }

  auto consumed = static_cast<std::size_t>(parsed);
  if (consumed > limits.max_header_bytes) {
    return invalid(431);
  }
  if (num_headers > limits.max_headers) {
    return invalid(431);
  }
  if (minor_version < 0 || minor_version > 1) {
    return invalid(505);
  }

  parsed_head result;
  result.status = head_status::complete;
  result.consumed = consumed;
  result.head.minor_version = minor_version;
  result.head.method.assign(method, method_len);
  result.head.target.assign(target, target_len);
  result.head.headers.reserve(num_headers);

  for (std::size_t i = 0; i < num_headers; ++i) {
    if (headers[i].name == nullptr) {
      // Continuation line: obsolete line folding is not accepted.
      return invalid(400);
    }
    result.head.headers.push_back(name_value{
        std::string(headers[i].name, headers[i].name_len),
        std::string(headers[i].value, headers[i].value_len),
    });
  }

  bool has_host = false;
  bool has_transfer_encoding = false;
  bool has_content_length = false;
  std::size_t content_length = 0;

  for (const name_value& field : result.head.headers) {
    if (detail::ascii_iequals(field.name, "host")) {
      has_host = true;
    } else if (detail::ascii_iequals(field.name, "transfer-encoding")) {
      has_transfer_encoding = true;
    } else if (detail::ascii_iequals(field.name, "content-length")) {
      std::size_t value = 0;
      if (!parse_content_length(field.value, value)) {
        return invalid(400);
      }
      if (has_content_length && value != content_length) {
        return invalid(400);  // conflicting Content-Length headers
      }
      has_content_length = true;
      content_length = value;
    }
  }

  if (has_transfer_encoding && has_content_length) {
    return invalid(400);  // request smuggling guard
  }
  if (has_transfer_encoding) {
    return invalid(501);  // chunked bodies are out of scope
  }
  if (minor_version == 1 && !has_host) {
    return invalid(400);
  }
  if (content_length > limits.max_body_bytes) {
    return invalid(413);
  }

  bool keep_alive = minor_version == 1;
  for (const name_value& field : result.head.headers) {
    if (!detail::ascii_iequals(field.name, "connection")) {
      continue;
    }
    if (has_token(field.value, "close")) {
      keep_alive = false;
    } else if (has_token(field.value, "keep-alive")) {
      keep_alive = true;
    }
  }

  result.content_length = content_length;
  result.keep_alive = keep_alive;

  // picohttpparser hands back the raw target; decode the query string here so
  // handlers see application/x-www-form-urlencoded values (+ -> space, %XX).
  if (auto question = result.head.target.find('?'); question != std::string::npos) {
    std::string_view query(result.head.target.data() + question + 1,
                           result.head.target.size() - question - 1);
    ada::url_search_params params(query);
    result.head.query_params.reserve(params.size());
    for (const auto& [key, value] : params) {
      result.head.query_params.push_back(name_value{std::string(key), std::string(value)});
    }
  }

  return result;
}

}  // namespace coro_http
