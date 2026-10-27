#pragma once

#include <coroutine>

namespace coro_http {

// co_await current_handle() yields the handle of the coroutine that awaits it.
// It never actually suspends: await_suspend captures the handle and returns
// false, which tells the compiler to continue without handing control away.
struct current_handle_awaiter {
  std::coroutine_handle<> handle{};

  bool await_ready() const noexcept { return false; }

  bool await_suspend(std::coroutine_handle<> self) noexcept {
    handle = self;
    return false;
  }

  std::coroutine_handle<> await_resume() const noexcept { return handle; }
};

[[nodiscard]] inline current_handle_awaiter current_handle() noexcept { return {}; }

}  // namespace coro_http
