#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "http/response.h"
#include "http/server.h"
#include "runtime/blocking_call.h"

using namespace std::chrono_literals;

using coro_http::request;
using coro_http::response;
using coro_http::server;
using coro_http::server_config;
using coro_http::task;

namespace {

server_config test_config() {
  server_config config;
  config.port = 0;
  config.shutdown_grace = 500ms;
  config.keep_alive_timeout = 5000ms;
  return config;
}

class http_client {
public:
  explicit http_client(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd_ >= 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    REQUIRE(::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

    timeval timeout{};
    timeout.tv_sec = 5;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  }

  ~http_client() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  http_client(const http_client&) = delete;
  http_client& operator=(const http_client&) = delete;

  void send(std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
      ssize_t written = ::send(fd_, data.data() + offset, data.size() - offset, 0);
      REQUIRE(written > 0);
      offset += static_cast<std::size_t>(written);
    }
  }

  // Returns the whole response (head plus body) or an empty string on EOF.
  std::string read_response(bool expect_body = true) {
    for (;;) {
      const auto header_end = buffer_.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        const auto length = content_length(buffer_.substr(0, header_end));
        REQUIRE(length.has_value());
        const std::size_t total = header_end + 4 + (expect_body ? *length : 0);
        while (buffer_.size() < total) {
          if (!fill()) {
            return {};
          }
        }
        std::string result = buffer_.substr(0, total);
        buffer_.erase(0, total);
        return result;
      }
      if (!fill()) {
        return {};
      }
    }
  }

  bool connection_closed() {
    if (!buffer_.empty()) {
      return false;
    }
    char scratch = 0;
    ssize_t n = ::recv(fd_, &scratch, 1, 0);
    return n <= 0;
  }

private:
  static std::size_t header_value(std::string_view text, std::string_view name,
                                  std::size_t fallback) {
    const auto position = text.find(name);
    if (position == std::string_view::npos) {
      return fallback;
    }
    std::size_t cursor = position + name.size();
    while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == ':')) {
      ++cursor;
    }
    std::size_t value = 0;
    while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
      value = value * 10 + static_cast<std::size_t>(text[cursor] - '0');
      ++cursor;
    }
    return value;
  }

  static std::optional<std::size_t> content_length(std::string_view head) {
    if (head.find("Content-Length: ") == std::string_view::npos &&
        head.find("content-length: ") == std::string_view::npos) {
      return std::nullopt;
    }
    return header_value(head, "Content-Length: ", 0);
  }

  bool fill() {
    char chunk[4096];
    ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      return false;
    }
    buffer_.append(chunk, static_cast<std::size_t>(n));
    return true;
  }

  int fd_{-1};
  std::string buffer_;
};

class running_server {
public:
  explicit running_server(server_config config = test_config()) : app_(std::move(config)) {}
  ~running_server() { stop(); }

  server& app() { return app_; }
  std::uint16_t port() const { return app_.port(); }

  void start() {
    thread_ = std::thread([this] { app_.run(); });
  }

  void stop() {
    app_.request_stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  server app_;
  std::thread thread_;
};

std::string get_request(std::string_view target, bool close = false) {
  std::string text = "GET " + std::string(target) + " HTTP/1.1\r\nHost: test\r\n";
  if (close) {
    text += "Connection: close\r\n";
  }
  text += "\r\n";
  return text;
}

void add_hello_route(server& app) {
  app.routes().add("GET", "/hello", [](const request&) -> task<response> {
    co_return response::with_text("hello\n");
  });
}

}  // namespace

TEST_CASE("serves a GET over a real socket") {
  running_server fixture;
  add_hello_route(fixture.app());
  fixture.start();

  http_client client(fixture.port());
  client.send(get_request("/hello"));
  const std::string wire = client.read_response();

  REQUIRE(wire.starts_with("HTTP/1.1 200 OK\r\n"));
  REQUIRE(wire.ends_with("hello\n"));
}

TEST_CASE("keeps a connection alive for a second request") {
  running_server fixture;
  add_hello_route(fixture.app());
  fixture.start();

  http_client client(fixture.port());
  client.send(get_request("/hello"));
  REQUIRE(client.read_response().ends_with("hello\n"));

  client.send(get_request("/hello"));
  REQUIRE(client.read_response().ends_with("hello\n"));
}

TEST_CASE("POST bodies reach the handler") {
  running_server fixture;
  fixture.app().routes().add("POST", "/echo", [](const request& req) -> task<response> {
    co_return response::with_text(req.body);
  });
  fixture.start();

  http_client client(fixture.port());
  const std::string body = "payload";
  client.send("POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: " +
              std::to_string(body.size()) + "\r\n\r\n" + body);

  REQUIRE(client.read_response().ends_with(body));
}

TEST_CASE("unknown paths and methods get the right status") {
  running_server fixture;
  add_hello_route(fixture.app());
  fixture.start();

  http_client client(fixture.port());
  client.send(get_request("/missing"));
  REQUIRE(client.read_response().starts_with("HTTP/1.1 404 Not Found\r\n"));

  client.send("DELETE /hello HTTP/1.1\r\nHost: test\r\n\r\n");
  const std::string wire = client.read_response();
  REQUIRE(wire.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
  REQUIRE(wire.find("Allow: GET") != std::string::npos);
}

TEST_CASE("a slow handler does not block other connections") {
  running_server fixture;
  add_hello_route(fixture.app());
  fixture.app().routes().add("GET", "/slow", [&app = fixture.app()](const request&) -> task<response> {
    co_await app.loop().sleep_for(300ms);
    co_return response::with_text("slow\n");
  });
  fixture.start();

  http_client slow(fixture.port());
  slow.send(get_request("/slow"));
  std::this_thread::sleep_for(30ms);

  http_client fast(fixture.port());
  const auto started = std::chrono::steady_clock::now();
  fast.send(get_request("/hello"));
  REQUIRE(fast.read_response().ends_with("hello\n"));
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE(elapsed < 250ms);
  REQUIRE(slow.read_response().ends_with("slow\n"));
}

TEST_CASE("blocking_call works from a handler") {
  running_server fixture;
  fixture.app().routes().add("GET", "/work", [&app = fixture.app()](const request&) -> task<response> {
    auto result = co_await coro_http::blocking_call(app.pool(), app.loop(),
                                                    [] { return std::string("from-worker"); });
    if (!result) {
      co_return response::with_status(503);
    }
    co_return response::with_text(*result);
  });
  fixture.start();

  http_client client(fixture.port());
  client.send(get_request("/work"));
  REQUIRE(client.read_response().ends_with("from-worker"));
}

TEST_CASE("an oversized body is refused with 413") {
  server_config config = test_config();
  config.max_body_bytes = 32;
  running_server fixture(config);
  fixture.start();

  http_client client(fixture.port());
  client.send("POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 33\r\n\r\n");

  const std::string wire = client.read_response();
  REQUIRE(wire.starts_with("HTTP/1.1 413 Payload Too Large\r\n"));
}

TEST_CASE("a malformed request gets a 400 and the connection closes") {
  running_server fixture;
  add_hello_route(fixture.app());
  fixture.start();

  http_client client(fixture.port());
  client.send("BROKEN\r\n\r\n");

  REQUIRE(client.read_response().starts_with("HTTP/1.1 400 Bad Request\r\n"));
  REQUIRE(client.connection_closed());
}

TEST_CASE("shutdown returns while an idle keep-alive connection is open") {
  running_server fixture;
  add_hello_route(fixture.app());
  fixture.start();

  http_client client(fixture.port());
  client.send(get_request("/hello"));
  REQUIRE(client.read_response().ends_with("hello\n"));

  const auto started = std::chrono::steady_clock::now();
  fixture.stop();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE(elapsed < 2s);
}

TEST_CASE("the readiness backend serves a request") {
  server_config config = test_config();
  config.backend = coro_http::io_backend::readiness;

  running_server fixture(config);
  add_hello_route(fixture.app());
  fixture.start();

  http_client client(fixture.port());
  client.send(get_request("/hello"));
  REQUIRE(client.read_response().ends_with("hello\n"));
}

#if defined(CORO_HTTP_HAVE_IO_URING)
TEST_CASE("the io_uring backend serves a request when the kernel allows it") {
  server_config config = test_config();
  config.backend = coro_http::io_backend::uring;

  std::unique_ptr<running_server> fixture;
  try {
    fixture = std::make_unique<running_server>(config);
  } catch (const std::runtime_error&) {
    SKIP("io_uring is built but not available on this kernel");
  }

  add_hello_route(fixture->app());
  fixture->start();

  http_client client(fixture->port());
  client.send(get_request("/hello"));
  REQUIRE(client.read_response().ends_with("hello\n"));
}
#endif
