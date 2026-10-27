#include "http/server.h"

#include <atomic>
#include <chrono>
#include <memory>

#include "http/session.h"

namespace coro_http {

server::server(server_config config)
    : config_(std::move(config)),
      pool_(config_.workers, config_.worker_capacity),
      stop_event_(loop_),
      acceptor_(loop_, config_.host, config_.port) {}

server::~server() = default;

void server::run() {
  if (running_.exchange(true)) {
    return;
  }

  auto accepting = accept_loop();
  accept_handle_ = accepting.release();
  loop_.post(accept_handle_);

  auto watcher = watch_for_stop();
  loop_.post(watcher.handle);

  loop_.run();

  // The accept loop is parked on the listening fd until the loop stops; its
  // frame is ours to destroy once nothing can resume it.
  if (accept_handle_) {
    accept_handle_.destroy();
    accept_handle_ = {};
  }

  pool_.stop();
  running_.store(false);
}

void server::request_stop() {
  if (!running_.load()) {
    return;
  }
  stop_event_.set();
}

detail::detached_coro server::watch_for_stop() {
  co_await stop_event_;
  co_await shutdown();
}

task<void> server::accept_loop() {
  for (;;) {
    if (!acceptor_.is_open()) {
      co_return;
    }

    auto accepted = co_await acceptor_.accept();
    if (!accepted) {
      if (!acceptor_.is_open()) {
        co_return;
      }
      // Transient accept failure (EMFILE and friends): back off instead of
      // spinning on the loop.
      co_await loop_.sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    ++stats_.total_connections;
    auto connection = std::make_shared<session>(loop_, *accepted, routes_, config_, stats_);
    sessions_.insert(connection.get());

    // The callback runs after the session's frame is gone, so it is the safe
    // place to drop the last owner.
    spawn(loop_, connection->run(), [this, connection] {
      sessions_.erase(connection.get());
    });
  }
}

task<void> server::shutdown() {
  acceptor_.close();

  // Half-close every connection's read side: parked readers wake with EOF and
  // unwind on their own, without anyone resuming a coroutine frame by hand.
  for (session* connection : sessions_) {
    connection->begin_close();
  }

  const auto grace_end = loop_.now() + config_.shutdown_grace;
  while (!sessions_.empty() && loop_.now() < grace_end) {
    co_await loop_.sleep_for(std::chrono::milliseconds(10));
  }

  if (!sessions_.empty()) {
    // A handler ignored the signal for too long; drop the sockets.
    for (session* connection : sessions_) {
      connection->force_close();
    }
    const auto hard_end = loop_.now() + config_.shutdown_grace;
    while (!sessions_.empty() && loop_.now() < hard_end) {
      co_await loop_.sleep_for(std::chrono::milliseconds(10));
    }
  }

  loop_.stop();
}

}  // namespace coro_http
