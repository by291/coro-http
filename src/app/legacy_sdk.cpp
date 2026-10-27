#include "app/legacy_sdk.h"

#include <chrono>
#include <functional>
#include <thread>

namespace coro_http::legacy {

std::string fetch(const std::string& key) {
  // A blocking round trip to "somewhere else" that we cannot turn into an fd.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto hash = std::hash<std::string>{}(key);
  return "legacy[" + key + "]=" + std::to_string(hash % 1'000'000);
}

}  // namespace coro_http::legacy
