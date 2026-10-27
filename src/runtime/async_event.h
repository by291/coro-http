#pragma once

#include <coroutine>
#include <mutex>
#include <utility>
#include <vector>

#include "runtime/scheduler.h"

namespace coro_http {

// A one-shot, thread-safe event. Coroutines await it on the loop thread; other
// threads (signal watchers, tests) set() it. Waiters are resumed through the
// scheduler, never on the setting thread.
class async_event {
public:
  explicit async_event(scheduler& sched) : sched_(sched) {}

  void set() {
    std::vector<std::coroutine_handle<>> to_wake;
    {
      std::lock_guard lock(mutex_);
      if (set_) {
        return;
      }
      set_ = true;
      to_wake.swap(waiters_);
    }
    for (std::coroutine_handle<> handle : to_wake) {
      sched_.post(handle);
    }
  }

  [[nodiscard]] bool is_set() const {
    std::lock_guard lock(mutex_);
    return set_;
  }

  struct awaiter {
    async_event& event;

    [[nodiscard]] bool await_ready() const {
      std::lock_guard lock(event.mutex_);
      return event.set_;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
      std::lock_guard lock(event.mutex_);
      if (event.set_) {
        return false;  // set between await_ready and here
      }
      event.waiters_.push_back(handle);
      return true;
    }

    void await_resume() const noexcept {}
  };

  [[nodiscard]] awaiter wait() { return awaiter{*this}; }
  [[nodiscard]] awaiter operator co_await() { return awaiter{*this}; }

private:
  scheduler& sched_;
  mutable std::mutex mutex_;
  bool set_{false};
  std::vector<std::coroutine_handle<>> waiters_;
};

}  // namespace coro_http
