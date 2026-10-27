#pragma once

namespace coro_http {

// Portable stand-in for eventfd: the thread pool and signal handlers write one
// byte to wake the loop out of poll(). Both ends are non-blocking, so signal()
// is async-signal-safe (a write to a full pipe fails with EAGAIN and that is
// fine - a pending byte already wakes the loop).
class wakeup_pipe {
public:
  wakeup_pipe();
  ~wakeup_pipe();

  wakeup_pipe(const wakeup_pipe&) = delete;
  wakeup_pipe& operator=(const wakeup_pipe&) = delete;

  int read_fd() const { return read_fd_; }

  void signal() noexcept;
  void drain() noexcept;

private:
  int read_fd_{-1};
  int write_fd_{-1};
};

}  // namespace coro_http
