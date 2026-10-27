#pragma once

#include <coroutine>

namespace coro_http {

// The only thing the coroutine machinery needs from the outside world: a way
// to put a suspended coroutine back on the thread that owns the loop.
class scheduler {
public:
  virtual ~scheduler() = default;

  // Must be safe to call from any thread. The handle is resumed on the loop
  // thread, never by the caller.
  virtual void post(std::coroutine_handle<> handle) = 0;
};

}  // namespace coro_http
