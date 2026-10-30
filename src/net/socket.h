#pragma once

#include <chrono>
#include <expected>
#include <span>
#include <string_view>
#include <system_error>

#include "net/io_engine.h"
#include "runtime/task.h"

namespace coro_http {

// A TCP socket whose IO reads like synchronous code:
//
//   auto n = co_await socket.read_some(buffer);
//
// The socket owns the fd and forwards operations to the io_engine, so the
// same call site works over epoll/kqueue readiness or io_uring completions.
class stream_socket {
public:
  stream_socket(io_engine& engine, int fd);
  ~stream_socket();

  stream_socket(stream_socket&& other) noexcept;
  stream_socket& operator=(stream_socket&& other) noexcept;

  stream_socket(const stream_socket&) = delete;
  stream_socket& operator=(const stream_socket&) = delete;

  [[nodiscard]] int fd() const { return fd_; }
  [[nodiscard]] bool is_open() const { return fd_ >= 0; }

  // Cancels whatever is pending on the fd, then closes it.
  void close();

  // Half-close so a parked reader sees EOF while writes still work.
  void shutdown_read();

  // 0 means the peer closed the connection.
  [[nodiscard]] task<std::expected<std::size_t, std::error_code>> read_some(
      std::span<char> buffer,
      io_engine::clock::time_point deadline = io_engine::clock::time_point::max());

  [[nodiscard]] task<std::expected<void, std::error_code>> write_all(
      std::string_view data,
      io_engine::clock::time_point deadline = io_engine::clock::time_point::max());

private:
  io_engine* engine_;
  int fd_;
};

}  // namespace coro_http
