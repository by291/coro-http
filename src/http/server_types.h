#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "net/io_engine.h"

namespace coro_http {

struct server_config {
  std::string host{"127.0.0.1"};
  std::uint16_t port{8080};

  // auto_select picks io_uring when built with it and the kernel supports it,
  // otherwise the epoll/kqueue readiness engine.
  io_backend backend{io_backend::auto_select};

  // Threads that exist only for blocking_call().
  std::size_t workers{2};
  std::size_t worker_capacity{1024};

  std::size_t max_body_bytes{1 << 20};
  std::chrono::milliseconds keep_alive_timeout{30000};
  std::chrono::milliseconds header_timeout{5000};
  std::chrono::milliseconds body_timeout{10000};
  std::chrono::milliseconds write_timeout{10000};
  std::chrono::milliseconds shutdown_grace{5000};

  bool access_log{false};
};

struct server_stats {
  std::atomic<std::uint64_t> requests{0};
  std::atomic<std::uint64_t> active_connections{0};
  std::atomic<std::uint64_t> total_connections{0};
};

}  // namespace coro_http
