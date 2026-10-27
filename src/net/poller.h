#pragma once

#include <memory>

namespace coro_http {

// Interest bits, shared by both backends.
enum : unsigned { io_read = 1u << 0, io_write = 1u << 1 };

// Readiness bits reported by wait(). Level-triggered: as long as the condition
// holds, the fd is reported again.
enum : unsigned {
  io_event_readable = 1u << 0,
  io_event_writable = 1u << 1,
  io_event_error = 1u << 2,
  io_event_hangup = 1u << 3,
};

struct io_event {
  int fd{-1};
  unsigned flags{0};
};

// The entire platform surface: kqueue on macOS, epoll on Linux. Timers are
// deliberately not part of this interface - epoll has none, so the event loop
// keeps a deadline heap and only asks the backend "how long may I sleep".
class poller {
public:
  virtual ~poller() = default;

  // Level-triggered interest for one fd. Replaces whatever was registered.
  virtual bool watch(int fd, unsigned interests) = 0;
  virtual void unwatch(int fd) = 0;

  // timeout_ms < 0 blocks indefinitely. Returns the number of events, 0 on
  // timeout, or -1 on error (errno is set).
  virtual int wait(io_event* events, int max_events, int timeout_ms) = 0;
};

std::unique_ptr<poller> make_poller();

}  // namespace coro_http
