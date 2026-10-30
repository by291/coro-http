#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <system_error>

#include "net/event_loop.h"
#include "net/io_engine.h"
#include "runtime/task.h"

namespace coro_http {

// IPv4 listening socket. Binding happens in the constructor so the real port
// is known immediately, which is what tests need when they ask for port 0.
class acceptor {
public:
  acceptor(event_loop& loop, std::string host, std::uint16_t port, int backlog = 128);
  ~acceptor();

  acceptor(const acceptor&) = delete;
  acceptor& operator=(const acceptor&) = delete;

  [[nodiscard]] task<std::expected<int, std::error_code>> accept();

  void close();

  [[nodiscard]] int fd() const { return fd_; }
  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] bool is_open() const { return fd_ >= 0; }

private:
  io_engine& engine_;
  int fd_{-1};
  std::uint16_t port_{0};
};

}  // namespace coro_http
