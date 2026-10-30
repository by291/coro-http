#include "http/session.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <span>
#include <utility>

#include "http/response.h"
#include "runtime/current_handle.h"

namespace coro_http {

session::session(event_loop& loop, int fd, const router& routes, const server_config& config,
                 server_stats& stats)
    : loop_(loop),
      socket_(loop.engine(), fd),
      routes_(routes),
      config_(config),
      stats_(stats) {
  limits_.max_body_bytes = config_.max_body_bytes;
}

task<void> session::run() {
  ++stats_.active_connections;

  struct active_guard {
    std::atomic<std::uint64_t>& counter;
    ~active_guard() { counter.fetch_sub(1, std::memory_order_relaxed); }
  } guard{stats_.active_connections};

  while (!closing_) {
    read_outcome outcome = co_await read_request();

    if (outcome.status == read_outcome::kind::protocol_error) {
      std::string wire =
          serialize_response(response::with_status(outcome.error_status), false, false);
      co_await socket_.write_all(wire, loop_.now() + config_.write_timeout);
      break;
    }
    if (outcome.status != read_outcome::kind::request) {
      break;
    }

    stats_.requests.fetch_add(1, std::memory_order_relaxed);

    const bool head_request = outcome.req.method == "HEAD";
    if (config_.access_log) {
      std::fprintf(stderr, "%s %s\n", outcome.req.method.c_str(), outcome.req.target.c_str());
    }

    // Shutdown wakes this frame through its socket, never by resuming it, so
    // a handler parked in blocking_call() is left alone until its worker
    // returns.
    response res = co_await routes_.dispatch(std::move(outcome.req));

    const bool keep_alive = outcome.keep_alive && !closing_;
    std::string wire = serialize_response(res, keep_alive, head_request);
    auto written = co_await socket_.write_all(wire, loop_.now() + config_.write_timeout);
    if (!written || !keep_alive) {
      break;
    }
  }

  socket_.close();
}

task<session::read_outcome> session::read_request() {
  for (;;) {
    if (closing_) {
      co_return read_outcome{};
    }

    compact();
    parsed_head parsed = parse_request_head(buffer_, limits_);

    if (parsed.status == head_status::invalid) {
      read_outcome outcome;
      outcome.status = read_outcome::kind::protocol_error;
      outcome.error_status = parsed.error_status;
      co_return outcome;
    }

    if (parsed.status == head_status::complete) {
      const std::size_t head_end = parsed.consumed;
      const std::size_t total = head_end + parsed.content_length;
      while (buffer_.size() < total) {
        auto more = co_await fill(config_.body_timeout);
        if (!more) {
          read_outcome outcome;
          outcome.status = read_outcome::kind::io_error;
          co_return outcome;
        }
        if (*more == 0) {
          co_return read_outcome{};
        }
      }

      request req = std::move(parsed.head);
      req.body.assign(buffer_, head_end, parsed.content_length);
      consumed_ = total;

      read_outcome outcome;
      outcome.status = read_outcome::kind::request;
      outcome.req = std::move(req);
      outcome.keep_alive = parsed.keep_alive;
      co_return outcome;
    }

    // Incomplete head: idle connections get the longer keep-alive budget, an
    // in-flight request head gets the header budget.
    const bool idle = buffer_.empty();
    auto more = co_await fill(idle ? config_.keep_alive_timeout : config_.header_timeout);
    if (!more) {
      read_outcome outcome;
      outcome.status = read_outcome::kind::io_error;
      co_return outcome;
    }
    if (*more == 0) {
      co_return read_outcome{};
    }
  }
}

task<std::expected<std::size_t, std::error_code>> session::fill(std::chrono::milliseconds timeout) {
  std::array<char, 4096> chunk{};
  auto read = co_await socket_.read_some(std::span<char>(chunk.data(), chunk.size()),
                                         loop_.now() + timeout);
  if (!read) {
    co_return std::move(read);
  }
  if (*read != 0) {
    buffer_.append(chunk.data(), *read);
  }
  co_return *read;
}

void session::compact() {
  if (consumed_ == 0) {
    return;
  }
  buffer_.erase(0, consumed_);
  consumed_ = 0;
}

void session::begin_close() {
  closing_ = true;
  // Half-closing the read side wakes a coroutine parked on read_some() with
  // EOF. Nothing touches the coroutine frame itself, so this is safe even
  // while a handler is running.
  socket_.shutdown_read();
}

void session::force_close() { socket_.close(); }

}  // namespace coro_http
