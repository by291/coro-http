#include "http/response.h"

#include <string>
#include <utility>

namespace coro_http {

response response::with_text(std::string body, int status) {
  response res;
  res.status = status;
  res.body = std::move(body);
  res.set_header("Content-Type", "text/plain; charset=utf-8");
  return res;
}

response response::with_status(int status) {
  response res;
  res.status = status;
  return res;
}

std::string_view response::header(std::string_view name) const {
  for (const header_field& field : headers) {
    if (detail::ascii_iequals(field.name, name)) {
      return field.value;
    }
  }
  return {};
}

void response::set_header(std::string name, std::string value) {
  for (header_field& field : headers) {
    if (detail::ascii_iequals(field.name, name)) {
      field.value = std::move(value);
      return;
    }
  }
  headers.push_back(header_field{std::move(name), std::move(value)});
}

std::string status_text(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 408:
      return "Request Timeout";
    case 413:
      return "Payload Too Large";
    case 414:
      return "URI Too Long";
    case 431:
      return "Request Header Fields Too Large";
    case 500:
      return "Internal Server Error";
    case 501:
      return "Not Implemented";
    case 503:
      return "Service Unavailable";
    case 505:
      return "HTTP Version Not Supported";
    default:
      return "Unknown";
  }
}

std::string serialize_response(const response& res, bool keep_alive, bool head_request) {
  std::string wire;
  wire.reserve(res.body.size() + 128);

  wire += "HTTP/1.1 ";
  wire += std::to_string(res.status);
  wire += ' ';
  wire += status_text(res.status);
  wire += "\r\n";

  bool has_content_length = false;
  for (const header_field& field : res.headers) {
    if (detail::ascii_iequals(field.name, "connection")) {
      continue;  // framing owns this one
    }
    if (detail::ascii_iequals(field.name, "content-length")) {
      has_content_length = true;
    }
    wire += field.name;
    wire += ": ";
    wire += field.value;
    wire += "\r\n";
  }

  if (!has_content_length) {
    wire += "Content-Length: ";
    wire += std::to_string(res.body.size());
    wire += "\r\n";
  }

  wire += (keep_alive && !res.close) ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
  wire += "\r\n";

  if (!head_request) {
    wire += res.body;
  }
  return wire;
}

}  // namespace coro_http
