#pragma once

#include <coroutine>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>

#include "runtime/scheduler.h"
#include "runtime/task.h"
#include "runtime/thread_pool.h"

namespace coro_http {

namespace detail {

template <class Result>
struct blocking_call_state {
  scheduler* sched{nullptr};
  std::mutex mutex;
  std::optional<Result> result;
  std::coroutine_handle<> waiter{};
};

// Suspends the coroutine until a worker fills in the result. The worker never
// touches the coroutine frame; it only stores a value and posts the handle.
template <class Result>
struct blocking_call_awaiter {
  std::shared_ptr<blocking_call_state<Result>> state;
  bool suspended{false};

  bool await_ready() {
    std::lock_guard lock(state->mutex);
    return state->result.has_value();
  }

  bool await_suspend(std::coroutine_handle<> handle) {
    std::lock_guard lock(state->mutex);
    if (state->result.has_value()) {
      return false;  // the worker finished before we got here
    }
    state->waiter = handle;
    suspended = true;
    return true;
  }

  void await_resume() noexcept {}

  ~blocking_call_awaiter() {
    if (!suspended) {
      return;
    }
    std::lock_guard lock(state->mutex);
    if (state->waiter) {
      state->waiter = {};
    }
  }
};

}  // namespace detail

// Runs a synchronous callable on the thread pool and resumes the coroutine on
// the loop thread once it is done.
//
// The calling convention is deliberate: this is the only place in the codebase
// where a blocking call is allowed, so `grep blocking_call` audits them all.
// A saturated pool is reported as an error rather than as backpressure, so the
// event loop never waits for a worker.
template <class F>
auto blocking_call(thread_pool& pool, scheduler& sched, F job)
    -> task<std::expected<std::invoke_result_t<F>, std::error_code>> {
  using result_type = std::invoke_result_t<F>;
  static_assert(!std::is_void_v<result_type>,
                "blocking_call needs a callable that returns a value");

  auto state = std::make_shared<detail::blocking_call_state<result_type>>();
  state->sched = &sched;

  // The callable is shared so the job stays copyable for std::function even
  // when the callable itself is move-only.
  auto job_holder = std::make_shared<F>(std::move(job));

  bool accepted = pool.try_submit([state, job_holder]() mutable {
    result_type produced = std::invoke(std::move(*job_holder));

    std::coroutine_handle<> waiter;
    {
      std::lock_guard lock(state->mutex);
      state->result.emplace(std::move(produced));
      waiter = std::exchange(state->waiter, {});
    }
    if (waiter) {
      state->sched->post(waiter);
    }
  });

  if (!accepted) {
    co_return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  co_await detail::blocking_call_awaiter<result_type>{state};
  co_return std::move(*state->result);
}

}  // namespace coro_http
