#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace coro_http {

template <class T = void>
class task;

namespace detail {

// Everything the promise types share. The important bits are the lazy start
// (initial_suspend) and symmetric transfer at final_suspend: resuming or
// finishing a chain of awaiters never grows the call stack.
struct task_promise_base {
  std::coroutine_handle<> continuation{};

  struct final_awaiter {
    bool await_ready() const noexcept { return false; }

    template <class Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> self) noexcept {
      auto continuation = self.promise().continuation;
      return continuation ? continuation : std::noop_coroutine();
    }

    void await_resume() const noexcept {}
  };

  std::suspend_always initial_suspend() const noexcept { return {}; }
  final_awaiter final_suspend() const noexcept { return {}; }

  // Errors are values in this codebase; a thrown exception is a bug.
  void unhandled_exception() const noexcept { std::terminate(); }
};

}  // namespace detail

template <class T>
struct task_promise final : detail::task_promise_base {
  std::optional<T> result;

  task<T> get_return_object() noexcept;

  template <class U>
    requires std::constructible_from<T, U&&>
  void return_value(U&& value) {
    result.emplace(std::forward<U>(value));
  }
};

template <>
struct task_promise<void> final : detail::task_promise_base {
  task<void> get_return_object() noexcept;
  void return_void() const noexcept {}
};

// A single-owner coroutine handle. Awaiting a task transplants its coroutine's
// continuation and lets symmetric transfer drive the chain; the frame dies
// with the temporary produced by operator co_await().
template <class T>
class [[nodiscard]] task {
public:
  using promise_type = task_promise<T>;

  explicit task(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

  task(task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  task& operator=(task&& other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  task(const task&) = delete;
  task& operator=(const task&) = delete;

  ~task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  // Only an rvalue task can be awaited, so a task can never be awaited twice.
  auto operator co_await() && noexcept {
    return awaiter{std::exchange(handle_, {})};
  }

  // For tests and for code that needs to observe the frame (e.g. the session
  // registry). The caller stops owning the frame.
  [[nodiscard]] std::coroutine_handle<promise_type> release() noexcept {
    return std::exchange(handle_, {});
  }

private:
  struct awaiter {
    std::coroutine_handle<promise_type> handle;

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
      handle.promise().continuation = continuation;
      return handle;  // symmetric transfer into the task
    }

    T await_resume() {
      if constexpr (std::is_void_v<T>) {
        return;
      } else {
        return std::move(*handle.promise().result);
      }
    }

    // The co_await full-expression ends here: the task frame is done with.
    ~awaiter() {
      if (handle) {
        handle.destroy();
      }
    }
  };

  std::coroutine_handle<promise_type> handle_;
};

template <class T>
task<T> task_promise<T>::get_return_object() noexcept {
  return task<T>{std::coroutine_handle<task_promise>::from_promise(*this)};
}

inline task<void> task_promise<void>::get_return_object() noexcept {
  return task<void>{std::coroutine_handle<task_promise<void>>::from_promise(*this)};
}

}  // namespace coro_http
