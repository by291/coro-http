#include "runtime/thread_pool.h"

#include <utility>

namespace coro_http {

thread_pool::thread_pool(std::size_t workers, std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {
  if (workers == 0) {
    workers = 1;
  }
  workers_.reserve(workers);
  for (std::size_t i = 0; i < workers; ++i) {
    workers_.emplace_back([this] { worker_loop(); });
  }
}

thread_pool::~thread_pool() { stop(); }

bool thread_pool::try_submit(std::function<void()> job) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || pending_ >= capacity_) {
      return false;
    }
    ++pending_;
    jobs_.push_back(std::move(job));
  }
  work_available_.notify_one();
  return true;
}

void thread_pool::stop() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_ && workers_.empty()) {
      return;
    }
    stopping_ = true;
  }
  work_available_.notify_all();

  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

std::size_t thread_pool::pending() const {
  std::lock_guard lock(mutex_);
  return pending_;
}

void thread_pool::worker_loop() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock lock(mutex_);
      work_available_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty()) {
        return;  // stopping_ is set and everything queued has been drained
      }
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }

    job();

    {
      std::lock_guard lock(mutex_);
      --pending_;
    }
  }
}

}  // namespace coro_http
