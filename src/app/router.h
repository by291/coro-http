#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "http/request.h"
#include "http/response.h"
#include "runtime/task.h"

namespace coro_http {

// A small table-driven router: exact segments plus :name parameters. Handlers
// are ordinary coroutines, so they can await timers or blocking_call.
class router {
public:
  using handler = std::function<task<response>(const request&)>;

  void add(std::string method, std::string pattern, handler handle);

  // Takes the request by value: the handler receives a reference to a request
  // owned by this coroutine's frame, which outlives the handler.
  [[nodiscard]] task<response> dispatch(request req) const;

private:
  struct route {
    std::string method;
    std::string pattern;
    handler handle;
  };

  std::vector<route> routes_;
};

}  // namespace coro_http
