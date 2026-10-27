#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <unordered_set>
#include <utility>

#include "app/router.h"
#include "http/server_types.h"
#include "net/acceptor.h"
#include "net/event_loop.h"
#include "runtime/async_event.h"
#include "runtime/spawn.h"
#include "runtime/task.h"
#include "runtime/thread_pool.h"

namespace coro_http {

class session;

// Owns the loop, the thread pool, the routes and every live session.
class server {
public:
  explicit server(server_config config = {});
  ~server();

  server(const server&) = delete;
  server& operator=(const server&) = delete;

  // Blocks on this thread until request_stop() is called.
  void run();

  // Thread-safe; safe to call more than once.
  void request_stop();

  [[nodiscard]] event_loop& loop() { return loop_; }
  [[nodiscard]] thread_pool& pool() { return pool_; }
  [[nodiscard]] router& routes() { return routes_; }
  [[nodiscard]] server_stats& stats() { return stats_; }
  [[nodiscard]] std::uint16_t port() const { return acceptor_.port(); }

private:
  [[nodiscard]] task<void> accept_loop();
  [[nodiscard]] task<void> shutdown();
  [[nodiscard]] detail::detached_coro watch_for_stop();

  server_config config_;
  event_loop loop_;
  thread_pool pool_;
  async_event stop_event_;
  router routes_;
  acceptor acceptor_;
  server_stats stats_;

  std::unordered_set<session*> sessions_;
  std::coroutine_handle<> accept_handle_{};
  std::atomic<bool> running_{false};
};

}  // namespace coro_http
