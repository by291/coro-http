#include "app/handlers.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <string>
#include <string_view>

#include "app/legacy_sdk.h"
#include "http/response.h"
#include "http/server.h"
#include "runtime/blocking_call.h"

namespace coro_http {

namespace {

constexpr std::chrono::milliseconds kMaxSlowDelay{60'000};

long parse_long(std::string_view text, long fallback) {
  long value = fallback;
  auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || ptr != text.data() + text.size()) {
    return fallback;
  }
  return value;
}

}  // namespace

void register_demo_routes(server& app) {
  app.routes().add("GET", "/hello", [](const request&) -> task<response> {
    co_return response::with_text("hello\n");
  });

  app.routes().add("POST", "/echo", [](const request& req) -> task<response> {
    co_return response::with_text(req.body);
  });

  // A timer suspension: while this request sleeps, every other connection
  // keeps making progress on the same thread.
  app.routes().add("GET", "/slow", [&app](const request& req) -> task<response> {
    long requested = parse_long(req.query_param("ms"), 100);
    auto delay = std::chrono::milliseconds(
        std::clamp<long>(requested, 0, kMaxSlowDelay.count()));
    co_await app.loop().sleep_for(delay);
    co_return response::with_text("slept " + std::to_string(delay.count()) + "ms\n");
  });

  // The blocking bridge. The SDK call has no fd and cannot be made async, so
  // it runs on the pool while the loop keeps serving.
  app.routes().add("GET", "/legacy", [&app](const request& req) -> task<response> {
    std::string key(req.query_param("key"));
    auto result = co_await blocking_call(app.pool(), app.loop(),
                                         [key] { return legacy::fetch(key); });
    if (!result) {
      co_return response::with_status(503);
    }
    co_return response::with_text(*result + "\n");
  });

  app.routes().add("GET", "/stats", [&app](const request&) -> task<response> {
    const auto requests = app.stats().requests.load();
    const auto active = app.stats().active_connections.load();
    const auto total = app.stats().total_connections.load();
    co_return response::with_text("requests=" + std::to_string(requests) +
                                  "\nactive_connections=" + std::to_string(active) +
                                  "\ntotal_connections=" + std::to_string(total) + "\n");
  });
}

}  // namespace coro_http
