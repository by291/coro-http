#pragma once

#include <coroutine>
#include <exception>
#include <utility>

#include "runtime/scheduler.h"
#include "runtime/task.h"

namespace coro_http {

namespace detail {

// The frame of a detached coroutine frees itself at final_suspend, so nothing
// ever needs to own or destroy it.
struct detached_coro {
  std::coroutine_handle<> handle;

  struct promise_type {
    detached_coro get_return_object() noexcept {
      return detached_coro{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() const noexcept { return {}; }
    std::suspend_never final_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    void unhandled_exception() const noexcept { std::terminate(); }
  };
};

}  // namespace detail

// Starts a task on the scheduler and forgets about it. The optional completion
// callback runs after the task's own frame has been destroyed, which makes it
// the safe place to drop the last owner of whatever the task referred to.
//
// Trap worth knowing: a coroutine lambda stores a pointer to its closure in
// the coroutine frame. `spawn(loop, [&]{...}())` destroys that closure at the
// end of the statement, so the suspended coroutine later reads a dangling
// `this`. Always keep the lambda in a named local.
inline void spawn(scheduler& sched, task<void> work) {
  auto driver = [](task<void> work) -> detail::detached_coro {
    co_await std::move(work);
  }(std::move(work));
  sched.post(driver.handle);
}

template <class OnDone>
void spawn(scheduler& sched, task<void> work, OnDone on_done) {
  auto driver = [](task<void> work, OnDone on_done) -> detail::detached_coro {
    co_await std::move(work);
    on_done();
  }(std::move(work), std::move(on_done));
  sched.post(driver.handle);
}

}  // namespace coro_http
