#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

#include "app/router.h"
#include "http/response.h"
#include "runtime/sync_wait.h"

using coro_http::request;
using coro_http::response;
using coro_http::router;
using coro_http::serialize_response;
using coro_http::sync_wait;
using coro_http::task;

namespace {

request make_request(std::string method, std::string target) {
  request req;
  req.method = std::move(method);
  req.target = std::move(target);
  req.minor_version = 1;
  return req;
}

}  // namespace

TEST_CASE("an exact route reaches its handler") {
  router routes;
  routes.add("GET", "/hello", [](const request&) -> task<response> {
    co_return response::with_text("hello");
  });

  response result = sync_wait(routes.dispatch(make_request("GET", "/hello")));

  REQUIRE(result.status == 200);
  REQUIRE(result.body == "hello");
}

TEST_CASE("a query string does not take part in routing") {
  router routes;
  routes.add("GET", "/hello", [](const request&) -> task<response> {
    co_return response::with_text("hello");
  });

  response result = sync_wait(routes.dispatch(make_request("GET", "/hello?name=x")));

  REQUIRE(result.status == 200);
}

TEST_CASE("path parameters are captured") {
  router routes;
  routes.add("GET", "/users/:id", [](const request& req) -> task<response> {
    std::string name = req.params.empty() ? std::string("<none>") : req.params[0].name;
    std::string value = req.params.empty() ? std::string("<none>") : req.params[0].value;
    co_return response::with_text(name + "=" + value);
  });

  response result = sync_wait(routes.dispatch(make_request("GET", "/users/42")));

  REQUIRE(result.status == 200);
  REQUIRE(result.body == "id=42");
}

TEST_CASE("an unknown path is a 404") {
  router routes;
  routes.add("GET", "/hello", [](const request&) -> task<response> {
    co_return response::with_text("hello");
  });

  response result = sync_wait(routes.dispatch(make_request("GET", "/missing")));

  REQUIRE(result.status == 404);
}

TEST_CASE("a known path with the wrong method is a 405 that lists Allow") {
  router routes;
  routes.add("GET", "/thing", [](const request&) -> task<response> {
    co_return response::with_text("thing");
  });
  routes.add("POST", "/thing", [](const request&) -> task<response> {
    co_return response::with_text("created", 201);
  });

  response result = sync_wait(routes.dispatch(make_request("DELETE", "/thing")));

  REQUIRE(result.status == 405);
  REQUIRE(result.header("allow") == "GET, POST");
}

TEST_CASE("HEAD falls back to the GET handler") {
  router routes;
  routes.add("GET", "/hello", [](const request&) -> task<response> {
    co_return response::with_text("hello");
  });

  response result = sync_wait(routes.dispatch(make_request("HEAD", "/hello")));

  REQUIRE(result.status == 200);
  REQUIRE(result.body == "hello");
}

TEST_CASE("responses serialize with framing headers") {
  response res = response::with_text("hi");
  std::string wire = serialize_response(res, true, false);

  REQUIRE(wire.starts_with("HTTP/1.1 200 OK\r\n"));
  REQUIRE(wire.find("Content-Length: 2\r\n") != std::string::npos);
  REQUIRE(wire.find("Connection: keep-alive\r\n") != std::string::npos);
  REQUIRE(wire.ends_with("\r\n\r\nhi"));
}

TEST_CASE("a HEAD response keeps Content-Length but drops the body") {
  response res = response::with_text("hello");
  std::string wire = serialize_response(res, false, true);

  REQUIRE(wire.find("Content-Length: 5\r\n") != std::string::npos);
  REQUIRE(wire.find("Connection: close\r\n") != std::string::npos);
  REQUIRE(wire.ends_with("\r\n\r\n"));
}

TEST_CASE("an error response carries its status text") {
  response res = response::with_status(404);
  std::string wire = serialize_response(res, true, false);

  REQUIRE(wire.starts_with("HTTP/1.1 404 Not Found\r\n"));
  REQUIRE(wire.find("Content-Length: 0\r\n") != std::string::npos);
}
