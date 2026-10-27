#pragma once

#include <string>

namespace coro_http::legacy {

// Stand-in for a vendor SDK we do not own: it offers a synchronous call, hands
// out no file descriptor, and cannot be made asynchronous. This is the kind of
// code blocking_call() exists for.
std::string fetch(const std::string& key);

}  // namespace coro_http::legacy
