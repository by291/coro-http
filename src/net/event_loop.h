#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "net/io_engine.h"
#include "runtime/scheduler.h"

namespace coro_http {

class event_loop;

using timer_id = std::uint64_t;

// Suspends until the fd reports the requested interest or the deadline passes.
// The registration is one-shot from the caller's point of view: it is removed
// when the awaiter resumes, even though the backend is level-triggered.
//
// Readiness awaiters belong to the readiness engine. A completion engine
// (io_uring) exposes operations through io_engine instead.
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
//
// The loop owns timers, the posted queue and the ready batch; the io_engine
// owns the kernel-facing model (readiness or completions).
class event_loop final : public scheduler {
public:
  using clock = std::chrono::steady_clock;

  explicit event_loop(io_backend backend = io_backend::auto_select);
  ~event_loop() override;

  event_loop(const event_loop&) = delete;
  event_loop& operator=(const event_loop&) = delete;

  // Blocks until stop() is called. Nested calls are not supported.
  void run();

  // Safe from any thread.
  void stop();

  void post(std::coroutine_handle<> handle) override;

  // Readiness registration; used by the readiness engine and its awaiters.
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

  // --- engine-facing API, loop thread only ---

  [[nodiscard]] io_engine& engine();
  [[nodiscard]] const io_engine& engine() const;

  // Queues a coroutine for the current run() batch, de-duplicating repeats.
  void push_ready(std::coroutine_handle<> handle);

  // Removes and returns the fd's pending readiness waiter, if any.
  [[nodiscard]] std::coroutine_handle<> take_waiter(int fd);

  [[nodiscard]] int wakeup_fd() const;
  void drain_wakeup();

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace coro_http
