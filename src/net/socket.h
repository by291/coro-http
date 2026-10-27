#pragma once

#include <chrono>
#include <coroutine>
#include <expected>
#include <span>
#include <string_view>
#include <system_error>

#include "net/event_loop.h"
#include "runtime/task.h"

namespace coro_http {

// A non-blocking TCP socket whose IO reads like synchronous code:
//
//   auto n = co_await socket.read_some(buffer);
//
// Under the hood each EAGAIN turns into a one-shot registration with the event
// loop, and the coroutine parks until the fd is ready.
class stream_socket {
public:
  stream_socket(event_loop& loop, int fd);
  ~stream_socket();

  stream_socket(stream_socket&& other) noexcept;
  stream_socket& operator=(stream_socket&& other) noexcept;

  stream_socket(const stream_socket&) = delete;
  stream_socket& operator=(const stream_socket&) = delete;

  [[nodiscard]] int fd() const { return fd_; }
  [[nodiscard]] bool is_open() const { return fd_ >= 0; }

  void close();

  // Half-close so a parked reader sees EOF while writes still work.
  void shutdown_read();

  // 0 means the peer closed the connection.
  [[nodiscard]] task<std::expected<std::size_t, std::error_code>> read_some(
      std::span<char> buffer,
      event_loop::clock::time_point deadline = event_loop::clock::time_point::max());

  [[nodiscard]] task<std::expected<void, std::error_code>> write_all(
      std::string_view data,
      event_loop::clock::time_point deadline = event_loop::clock::time_point::max());

private:
  event_loop* loop_;
  int fd_;
};

}  // namespace coro_http
