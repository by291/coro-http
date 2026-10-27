#pragma once

#include <chrono>
#include <coroutine>
#include <expected>
#include <string>
#include <system_error>

#include "app/router.h"
#include "http/parse_request_head.h"
#include "http/server_types.h"
#include "net/event_loop.h"
#include "net/socket.h"
#include "runtime/task.h"

namespace coro_http {

// One connection. run() is the showcase coroutine: it reads, dispatches and
// writes in a straight line even though every network wait suspends.
class session {
public:
  session(event_loop& loop, int fd, const router& routes, const server_config& config,
          server_stats& stats);

  [[nodiscard]] task<void> run();

  // Called only from the loop thread, during shutdown. Reading is fenced off,
  // which wakes a coroutine parked on read without touching its frame.
  void begin_close();

  // Last resort after the grace period: drop the socket.
  void force_close();

private:
  struct read_outcome {
    enum class kind { request, end_of_stream, protocol_error, io_error };
    kind status{kind::end_of_stream};
    request req;
    bool keep_alive{true};
    int error_status{0};
  };

  [[nodiscard]] task<read_outcome> read_request();

  // Appends to buffer_ and returns the number of bytes read (0 = EOF).
  [[nodiscard]] task<std::expected<std::size_t, std::error_code>> fill(
      std::chrono::milliseconds timeout);

  void compact();

  event_loop& loop_;
  stream_socket socket_;
  const router& routes_;
  const server_config& config_;
  server_stats& stats_;
  parse_limits limits_;

  std::string buffer_;
  std::size_t consumed_{0};
  bool closing_{false};
};

}  // namespace coro_http
