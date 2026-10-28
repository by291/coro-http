#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "http/parse_request_head.h"

using coro_http::head_status;
using coro_http::parse_limits;
using coro_http::parse_request_head;
using coro_http::parsed_head;

namespace {

parsed_head parse(std::string_view text) { return parse_request_head(text); }

parsed_head parse(std::string_view text, const parse_limits& limits) {
  return parse_request_head(text, limits);
}

}  // namespace

TEST_CASE("parses a minimal HTTP/1.1 request") {
  const std::string text = "GET /hello?x=1 HTTP/1.1\r\nHost: example\r\n\r\n";
  auto result = parse(text);

  REQUIRE(result.status == head_status::complete);
  REQUIRE(result.head.method == "GET");
  REQUIRE(result.head.target == "/hello?x=1");
  REQUIRE(result.head.path() == "/hello");
  REQUIRE(result.head.query() == "x=1");
  REQUIRE(result.head.minor_version == 1);
  REQUIRE(result.head.header("host") == "example");
  REQUIRE(result.head.header("HOST") == "example");
  REQUIRE(result.content_length == 0);
  REQUIRE(result.keep_alive);
  REQUIRE(result.consumed == text.size());
}

TEST_CASE("bare LF line endings are accepted") {
  auto result = parse("GET /hello HTTP/1.1\nHost: example\n\n");
  REQUIRE(result.status == head_status::complete);
  REQUIRE(result.head.path() == "/hello");
}

TEST_CASE("an incomplete head asks for more bytes") {
  auto result = parse("GET / HTTP/1.1\r\nHost: example\r\n");
  REQUIRE(result.status == head_status::need_more);
}

TEST_CASE("HTTP/1.0 closes by default") {
  auto result = parse("GET / HTTP/1.0\r\n\r\n");
  REQUIRE(result.status == head_status::complete);
  REQUIRE_FALSE(result.keep_alive);
}

TEST_CASE("HTTP/1.0 keeps the connection alive when asked") {
  auto result = parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
  REQUIRE(result.status == head_status::complete);
  REQUIRE(result.keep_alive);
}

TEST_CASE("HTTP/1.1 honours Connection: close") {
  auto result = parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  REQUIRE(result.status == head_status::complete);
  REQUIRE_FALSE(result.keep_alive);
}

TEST_CASE("HTTP/1.1 requires a Host header") {
  auto result = parse("GET / HTTP/1.1\r\n\r\n");
  REQUIRE(result.status == head_status::invalid);
  REQUIRE(result.error_status == 400);
}

TEST_CASE("POST carries a content length") {
  auto result = parse("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n");
  REQUIRE(result.status == head_status::complete);
  REQUIRE(result.content_length == 5);
}

TEST_CASE("chunked request bodies are not implemented") {
  auto result = parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
  REQUIRE(result.status == head_status::invalid);
  REQUIRE(result.error_status == 501);
}

TEST_CASE("Content-Length together with Transfer-Encoding is rejected") {
  auto result = parse(
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n");
  REQUIRE(result.status == head_status::invalid);
  REQUIRE(result.error_status == 400);
}

TEST_CASE("conflicting Content-Length headers are rejected") {
  auto result = parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n");
  REQUIRE(result.status == head_status::invalid);
  REQUIRE(result.error_status == 400);
}

TEST_CASE("identical Content-Length headers are accepted") {
  auto result = parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\n");
  REQUIRE(result.status == head_status::complete);
  REQUIRE(result.content_length == 3);
}

TEST_CASE("a malformed Content-Length is rejected") {
  auto result = parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n");
  REQUIRE(result.status == head_status::invalid);
  REQUIRE(result.error_status == 400);
}

TEST_CASE("negative Content-Length is rejected") {
  auto result = parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n");
  REQUIRE(result.status == head_status::invalid);
  REQUIRE(result.error_status == 400);
}

TEST_CASE("obsolete line folding is rejected") {
  auto result = parse("GET / HTTP/1.1\r\nHost: x\r\nX-Long: a\r\n b\r\n\r\n");
  REQUIRE(result.status == head_status::invalid);
  REQUIRE(result.error_status == 400);
}

TEST_CASE("methods are the router's business, not the parser's") {
  auto result = parse("DELETE /thing HTTP/1.1\r\nHost: x\r\n\r\n");
  REQUIRE(result.status == head_status::complete);
  REQUIRE(result.head.method == "DELETE");
}

TEST_CASE("HTTP versions above 1.1 are refused") {
  auto result = parse("GET / HTTP/2.0\r\nHost: x\r\n\r\n");
  REQUIRE(result.status == head_status::invalid);
  REQUIRE(result.error_status == 505);
}

TEST_CASE("garbage is rejected") {
  auto result = parse("this is not http\r\n\r\n");
  REQUIRE(result.status == head_status::invalid);
  REQUIRE(result.error_status == 400);
}

TEST_CASE("an oversized header block is refused with 431") {
  const std::string text =
      "GET / HTTP/1.1\r\nHost: x\r\nX-Big: " + std::string(40 * 1024, 'a') + "\r\n\r\n";
  auto result = parse(text);
  REQUIRE(result.status == head_status::invalid);
  REQUIRE(result.error_status == 431);
}

TEST_CASE("too many headers are refused with 431") {
  std::string text = "GET / HTTP/1.1\r\nHost: x\r\n";
  for (int i = 0; i < 120; ++i) {
    text += "X-Pad-" + std::to_string(i) + ": v\r\n";
  }
  text += "\r\n";

  parse_limits limits;
  limits.max_headers = 100;
  auto result = parse(text, limits);
  REQUIRE(result.status == head_status::invalid);
  REQUIRE(result.error_status == 431);
}

TEST_CASE("an oversized request line is refused with 414") {
  const std::string text =
      "GET /" + std::string(10 * 1024, 'a') + " HTTP/1.1\r\nHost: x\r\n\r\n";
  auto result = parse(text);
  REQUIRE(result.status == head_status::invalid);
  REQUIRE(result.error_status == 414);
}

TEST_CASE("a body larger than the limit is refused with 413") {
  parse_limits limits;
  limits.max_body_bytes = 16;
  auto result = parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 17\r\n\r\n", limits);
  REQUIRE(result.status == head_status::invalid);
  REQUIRE(result.error_status == 413);
}

TEST_CASE("query parameters are percent-decoded") {
  auto result = parse("GET /x?name=a%20b&plus=a+b&slash=%2F&upper=%41 HTTP/1.1\r\nHost: x\r\n\r\n");

  REQUIRE(result.status == head_status::complete);
  REQUIRE(result.head.query_param("name") == "a b");
  REQUIRE(result.head.query_param("plus") == "a b");
  REQUIRE(result.head.query_param("slash") == "/");
  REQUIRE(result.head.query_param("upper") == "A");
}

TEST_CASE("a query parameter may have no value and a missing key is empty") {
  auto result = parse("GET /x?flag&x=1&x=2 HTTP/1.1\r\nHost: x\r\n\r\n");

  REQUIRE(result.head.query_param("flag").empty());
  REQUIRE(result.head.query_param("x") == "1");
  REQUIRE(result.head.query_param("absent").empty());
}

TEST_CASE("query() still returns the raw, undecoded string") {
  auto result = parse("GET /x?a=b%20c HTTP/1.1\r\nHost: x\r\n\r\n");

  REQUIRE(result.head.query() == "a=b%20c");
  REQUIRE(result.head.query_param("a") == "b c");
}

TEST_CASE("a malformed percent escape is left alone") {
  auto result = parse("GET /x?a=%zz HTTP/1.1\r\nHost: x\r\n\r\n");

  REQUIRE(result.head.query_param("a") == "%zz");
}

TEST_CASE("an empty or absent query yields no parameters") {
  auto empty = parse("GET /x? HTTP/1.1\r\nHost: x\r\n\r\n");
  auto absent = parse("GET /x HTTP/1.1\r\nHost: x\r\n\r\n");

  REQUIRE(empty.status == head_status::complete);
  REQUIRE(empty.head.query_param("x").empty());
  REQUIRE(absent.head.query_param("x").empty());
}
