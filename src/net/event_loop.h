#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>

#include "net/poller.h"
#include "runtime/scheduler.h"

namespace coro_http {

class event_loop;

using timer_id = std::uint64_t;

// Suspends until the fd reports the requested interest or the deadline passes.
// The registration is one-shot from the caller's point of view: it is removed
// when the awaiter resumes, even though the backend is level-triggered.
class io_wait_awaiter {
public:
  io_wait_awaiter(event_loop& loop, int fd, unsigned interests,
                  std::chrono::steady_clock::time_point deadline);

  io_wait_awaiter(const io_wait_awaiter&) = delete;
  io_wait_awaiter& operator=(const io_wait_awaiter&) = delete;
  ~io_wait_awaiter();

  bool await_ready() const noexcept { return false; }
  bool await_suspend(std::coroutine_handle<> handle);
  bool await_resume();

private:
  event_loop& loop_;
  int fd_;
  unsigned interests_;
  std::chrono::steady_clock::time_point deadline_;
  timer_id timer_{0};
};

class sleep_awaiter {
public:
  sleep_awaiter(event_loop& loop, std::chrono::steady_clock::time_point deadline);

  sleep_awaiter(const sleep_awaiter&) = delete;
  sleep_awaiter& operator=(const sleep_awaiter&) = delete;
  ~sleep_awaiter();

  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> handle);
  void await_resume() noexcept {}

private:
  event_loop& loop_;
  std::chrono::steady_clock::time_point deadline_;
  timer_id timer_{0};
};

// Single-threaded reactor. All coroutines run on the thread that calls run();
// the only cross-thread entry point is post().
class event_loop final : public scheduler {
public:
  using clock = std::chrono::steady_clock;

  event_loop();
  ~event_loop() override;

  event_loop(const event_loop&) = delete;
  event_loop& operator=(const event_loop&) = delete;

  // Blocks until stop() is called. Nested calls are not supported.
  void run();

  // Safe from any thread, including a signal-adjacent context is *not* what
  // this promises: call it from ordinary thread code only.
  void stop();

  void post(std::coroutine_handle<> handle) override;

  void watch(int fd, unsigned interests, std::coroutine_handle<> handle);
  void unwatch(int fd);

  timer_id schedule(clock::time_point deadline, std::coroutine_handle<> handle);
  void cancel_timer(timer_id id);

  [[nodiscard]] io_wait_awaiter readable(int fd, clock::time_point deadline = clock::time_point::max());
  [[nodiscard]] io_wait_awaiter writable(int fd, clock::time_point deadline = clock::time_point::max());
  [[nodiscard]] sleep_awaiter sleep_until(clock::time_point deadline);
  [[nodiscard]] sleep_awaiter sleep_for(clock::duration duration) {
    return sleep_until(clock::now() + duration);
  }

  clock::time_point now() const { return clock::now(); }
  bool is_running() const;

  // Diagnostics: how many slots the deadline heap currently holds. Cancelled
  // entries are compacted away, so this stays proportional to live timers.
  std::size_t timer_entry_count() const;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace coro_http
