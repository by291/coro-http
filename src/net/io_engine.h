#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>

#include "runtime/task.h"

namespace coro_http {

class event_loop;

enum class io_backend {
  auto_select,
  readiness,  // epoll on Linux, kqueue on macOS: wait for readiness, then syscall
  uring,      // io_uring: submit the operation, wake on completion
};

// The seam between the event loop and the kernel's I/O model.
//
// A readiness engine does a non-blocking syscall and, on EAGAIN, parks the
// coroutine until the fd is ready. A completion engine (io_uring) submits the
// operation itself and parks the coroutine until the completion arrives. Both
// resume coroutines through event_loop::push_ready(), so the session code
// above this line cannot tell them apart.
class io_engine {
public:
  using clock = std::chrono::steady_clock;

  virtual ~io_engine() = default;

  io_engine(const io_engine&) = delete;
  io_engine& operator=(const io_engine&) = delete;

  // Human-readable backend name for logs.
  [[nodiscard]] virtual const char* name() const = 0;

  // Blocks until work arrives or the timeout expires, then hands the affected
  // coroutines to the loop. Returns false on a fatal error.
  virtual bool wait(int timeout_ms) = 0;

  // 0 means the peer closed the connection.
  virtual task<std::expected<std::size_t, std::error_code>> read_some(
      int fd, std::span<char> buffer, clock::time_point deadline) = 0;

  virtual task<std::expected<void, std::error_code>> write_all(
      int fd, std::string_view data, clock::time_point deadline) = 0;

  virtual task<std::expected<int, std::error_code>> accept(
      int listener, clock::time_point deadline) = 0;

  // Aborts whatever is pending on fd and wakes its coroutine with an error.
  // Must be called before the fd is closed, while the waiting frame is alive.
  virtual void cancel(int fd) = 0;

  // Readiness registration used by event_loop::readable()/writable(). A
  // completion engine has nothing to do here.
  virtual void watch_fd(int fd, unsigned interests) = 0;
  virtual void forget_fd(int fd) = 0;

protected:
  io_engine() = default;
};

// Completion engine when available and requested, readiness otherwise. An
// explicit io_backend::uring that cannot be honoured throws.
std::unique_ptr<io_engine> make_io_engine(io_backend backend, event_loop& loop);

#ifdef CORO_HTTP_HAVE_IO_URING
// Defined in uring_engine.cpp. Returns nullptr when the kernel refuses to
// create a ring (old kernel, seccomp, no memory), so auto_select can fall back.
std::unique_ptr<io_engine> make_uring_engine(event_loop& loop);
#endif

}  // namespace coro_http
