#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

#include "runtime/task.h"

namespace coro_http {

namespace detail {

// A tiny driver coroutine. It starts suspended, so sync_wait can resume it and
// let symmetric transfer run the whole await chain on the calling thread.
struct sync_wait_coro {
  struct promise_type {
    std::exception_ptr error;

    sync_wait_coro get_return_object() noexcept {
      return sync_wait_coro{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() const noexcept { return {}; }

    // The driver parks at final_suspend: sync_wait reads the stored exception
    // and destroys the frame; nothing resumes it afterwards.
    struct final_awaiter {
      bool await_ready() const noexcept { return false; }
      std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type>) const noexcept {
        return std::noop_coroutine();
      }
      void await_resume() const noexcept {}
    };

    final_awaiter final_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    void unhandled_exception() noexcept { error = std::current_exception(); }
  };

  std::coroutine_handle<promise_type> handle;
};

}  // namespace detail

// Runs a task to completion on the calling thread.
//
// Only for tests and benchmarks: it cannot make progress past an await that
// depends on an event loop. Real code hands tasks to an event_loop instead.
template <class T>
T sync_wait(task<T> work) {
  if constexpr (std::is_void_v<T>) {
    auto driver = [](task<void> work) -> detail::sync_wait_coro {
      co_await std::move(work);
    }(std::move(work));

    driver.handle.resume();
    auto error = driver.handle.promise().error;
    driver.handle.destroy();
    if (error) {
      std::rethrow_exception(error);
    }
  } else {
    std::optional<T> result;
    auto driver = [](task<T> work, std::optional<T>& out) -> detail::sync_wait_coro {
      out.emplace(co_await std::move(work));
    }(std::move(work), result);

    driver.handle.resume();
    auto error = driver.handle.promise().error;
    driver.handle.destroy();
    if (error) {
      std::rethrow_exception(error);
    }
    return std::move(*result);
  }
}

}  // namespace coro_http
