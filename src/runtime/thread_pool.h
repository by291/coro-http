#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace coro_http {

// The escape hatch for synchronous APIs that cannot be made async. Work is
// bounded: when the pool is full, try_submit() fails instead of making the
// event loop wait for a free worker.
class thread_pool {
public:
  explicit thread_pool(std::size_t workers = 2, std::size_t capacity = 1024);
  ~thread_pool();

  thread_pool(const thread_pool&) = delete;
  thread_pool& operator=(const thread_pool&) = delete;

  bool try_submit(std::function<void()> job);

  // Lets queued work finish, then joins the workers.
  void stop();

  // Jobs queued plus jobs currently running.
  std::size_t pending() const;

private:
  void worker_loop();

  mutable std::mutex mutex_;
  std::condition_variable work_available_;
  std::deque<std::function<void()>> jobs_;
  std::size_t pending_{0};
  std::size_t capacity_;
  bool stopping_{false};
  std::vector<std::thread> workers_;
};

}  // namespace coro_http
