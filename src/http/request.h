#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace coro_http {

struct header_field {
  std::string name;
  std::string value;
};

struct request {
  std::string method;
  std::string target;
  int minor_version{1};
  std::vector<header_field> headers;
  std::string body;

  // Filled by the router when the path pattern has :parameters.
  std::vector<header_field> params;

  // Decoded query parameters, filled by the parser when the target has a
  // query string. Unlike path()/query(), these are owned strings.
  std::vector<header_field> query_params;

  // Case-insensitive header lookup; returns an empty view when absent.
  [[nodiscard]] std::string_view header(std::string_view name) const;

  [[nodiscard]] std::string_view path() const;
  [[nodiscard]] std::string_view query() const;

  // Case-sensitive lookup over the decoded query parameters; returns an
  // empty view when absent.
  [[nodiscard]] std::string_view query_param(std::string_view name) const;
};

namespace detail {

inline char ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool ascii_iequals(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (ascii_lower(lhs[i]) != ascii_lower(rhs[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace detail

inline std::string_view request::header(std::string_view name) const {
  for (const header_field& field : headers) {
    if (detail::ascii_iequals(field.name, name)) {
      return field.value;
    }
  }
  return {};
}

inline std::string_view request::path() const {
  auto question = target.find('?');
  return std::string_view(target).substr(0, question == std::string::npos ? target.size() : question);
}

inline std::string_view request::query() const {
  auto question = target.find('?');
  if (question == std::string::npos) {
    return {};
  }
  return std::string_view(target).substr(question + 1);
}

inline std::string_view request::query_param(std::string_view name) const {
  for (const header_field& field : query_params) {
    if (field.name == name) {
      return field.value;
    }
  }
  return {};
}

}  // namespace coro_http
