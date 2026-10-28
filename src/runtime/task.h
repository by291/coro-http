#pragma once

#include <concepts>
#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

// How the compiler drives the coroutine types below.
//
// A function containing co_await/co_return becomes a state machine, and the
// compiler calls these hooks on its promise_type:
//
//   get_return_object()    before the body runs; produces the task object
//   initial_suspend()      awaited immediately; suspend_always keeps the task
//                          lazy, so the caller gets it before the body starts
//   return_value(v)        co_return v
//   return_void()          co_return;
//   unhandled_exception()  the body threw
//   final_suspend()        awaited after the body finishes; its awaiter
//                          decides whether the frame is freed or parked
//
// A co_await on a user awaiter calls await_ready(); if that returns false,
// await_suspend(handle) runs and its return type decides what happens next:
// void suspends, bool=false resumes immediately, and coroutine_handle<>
// hands control to that handle (symmetric transfer, no stack growth).
// await_resume() runs when the awaiting coroutine is resumed.
//
// coroutine_handle<P> is typed; coroutine_handle<> erases the promise type.
// from_promise() and promise() convert between a handle and its promise.

namespace coro_http {

template <class T = void>
class task;

namespace detail {

// Everything the promise types share. The task is lazy (initial_suspend
// suspends) and both edges of a co_await use symmetric transfer, so starting
// and finishing a chain never grows the call stack. final_suspend parks the
// frame so the awaiting coroutine can read the result and destroy it.
struct task_promise_base {
  // The coroutine awaiting this task; final_suspend transfers control back to
  // it. The awaiter installs it before entering the task, and it stays empty
  // for tasks that nobody awaits.
  std::coroutine_handle<> continuation{};

  struct final_awaiter {
    // final_suspend has to really suspend: the finished frame stays parked
    // here until the awaiter has taken the result (if any) and destroyed it.
    bool await_ready() const noexcept { return false; }

    // Returning a handle is symmetric transfer: resume the awaiter in place
    // instead of unwinding back to whoever resumed us. A task with no
    // continuation was resumed directly, so parking it lets resume() return.
    template <class Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> self) noexcept {
      auto continuation = self.promise().continuation;
      return continuation ? continuation : std::noop_coroutine();
    }

    // Required by the protocol, never called: after final_suspend the frame
    // is destroyed rather than resumed.
    void await_resume() const noexcept {}
  };

  // Lazy start: constructing a task runs nothing; the body starts when
  // somebody awaits the task (or resumes it explicitly).
  std::suspend_always initial_suspend() const noexcept { return {}; }
  final_awaiter final_suspend() const noexcept { return {}; }

  // Errors are values in this codebase; a thrown exception is a bug.
  void unhandled_exception() const noexcept { std::terminate(); }
};

}  // namespace detail

template <class T>
struct task_promise final : detail::task_promise_base {
  // Holds the body's value while the frame is parked at final_suspend;
  // await_resume moves it out before the frame is destroyed.
  std::optional<T> result;

  task<T> get_return_object() noexcept;

  // co_return value lands here; await_resume reads it back out.
  template <class U>
    requires std::constructible_from<T, U&&>
  void return_value(U&& value) {
    result.emplace(std::forward<U>(value));
  }
};

template <>
struct task_promise<void> final : detail::task_promise_base {
  task<void> get_return_object() noexcept;
  // co_return; lands here.
  void return_void() const noexcept {}
};

// A single-owner coroutine handle. Awaiting a task transplants its coroutine's
// continuation and lets symmetric transfer drive the chain; the frame dies
// with the temporary produced by operator co_await(). A task that is never
// awaited owns a suspended frame and destroys it when the task is dropped.
template <class T>
class [[nodiscard]] task {
public:
  // The compiler locates the coroutine promise through this name.
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

  // A task that was never awaited frees a frame still parked at
  // initial_suspend; if the body is suspended in an await, the awaiters it
  // owns unwind the chain beneath it.
  ~task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  // Only an rvalue task can be awaited, so a task can never be awaited twice:
  // the handle moves into the awaiter and the task is left empty.
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

    // The task has not started yet, so it cannot complete inline: always go
    // through await_suspend, which starts it via symmetric transfer.
    bool await_ready() const noexcept { return false; }

    // Install the resumption point before transferring control: when the body
    // finishes, final_suspend returns this handle.
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
      handle.promise().continuation = continuation;
      return handle;  // symmetric transfer into the task
    }

    T await_resume() {
      // The task is parked at final_suspend, so result is complete. Destroy
      // the finished child here rather than in the destructor. GCC keeps
      // co_await temporaries alive until the frame dies, so leaving the
      // handle armed would make tearing down an N-deep chain recurse N frames
      // deep. Destroying it now (and clearing the handle) keeps unwinding
      // iterative: every completed frame owns an empty awaiter.
      if constexpr (std::is_void_v<T>) {
        release_frame();
        return;
      } else {
        T value = std::move(*handle.promise().result);
        release_frame();
        return value;
      }
    }

    // Still needed when the awaiting coroutine is destroyed while suspended:
    // the chain beneath it is then torn down recursively. Keep await chains
    // shallow in code that can be cancelled.
    ~awaiter() { release_frame(); }

   private:
    void release_frame() {
      if (handle) {
        handle.destroy();
        handle = {};
      }
    }
  };

  std::coroutine_handle<promise_type> handle_;
};

// Called by the compiler right after the frame is created, before
// initial_suspend; the task it returns is what the caller receives.
template <class T>
task<T> task_promise<T>::get_return_object() noexcept {
  return task<T>{std::coroutine_handle<task_promise>::from_promise(*this)};
}

inline task<void> task_promise<void>::get_return_object() noexcept {
  return task<void>{std::coroutine_handle<task_promise<void>>::from_promise(*this)};
}

}  // namespace coro_http
