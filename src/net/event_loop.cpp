#include "net/event_loop.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <deque>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "net/poller.h"
#include "net/wakeup_pipe.h"

namespace coro_http {

namespace {

using clock_type = std::chrono::steady_clock;
using handle_type = std::coroutine_handle<>;

struct timer_entry {
  clock_type::time_point deadline;
  timer_id id;
};

struct later_deadline {
  bool operator()(const timer_entry& lhs, const timer_entry& rhs) const {
    return lhs.deadline > rhs.deadline;
  }
};

}  // namespace

struct event_loop::impl {
  wakeup_pipe wakeup_;
  std::unique_ptr<io_engine> engine_;

  std::unordered_map<int, handle_type> waiters_;

  std::priority_queue<timer_entry, std::vector<timer_entry>, later_deadline> timers_;
  std::unordered_map<timer_id, handle_type> timer_waiters_;
  timer_id next_timer_id_{1};
  std::size_t cancelled_timers_{0};

  // Lazy deletion keeps cancellation O(log n), but dead slots would otherwise
  // linger until their original deadline. Rebuild when they outnumber live
  // timers, which bounds the heap at roughly 2x the live set.
  void rebuild_timers() {
    std::vector<timer_entry> live;
    live.reserve(timer_waiters_.size());
    while (!timers_.empty()) {
      timer_entry entry = timers_.top();
      timers_.pop();
      if (timer_waiters_.find(entry.id) != timer_waiters_.end()) {
        live.push_back(entry);
      }
    }
    for (const timer_entry& entry : live) {
      timers_.push(entry);
    }
    cancelled_timers_ = 0;
  }

  std::mutex post_mutex_;
  std::deque<handle_type> posted_;

  std::atomic<bool> running_{false};

  // Coroutines the engine (and timers/posted queue) want resumed in this
  // iteration. queued_ de-duplicates handles within one batch.
  std::vector<handle_type> ready_;
  std::unordered_set<const void*> queued_;
};

event_loop::event_loop(io_backend backend) : impl_(std::make_unique<impl>()) {
  impl_->engine_ = make_io_engine(backend, *this);
}

event_loop::~event_loop() = default;

void event_loop::run() {
  impl_->running_.store(true, std::memory_order_release);

  while (impl_->running_.load(std::memory_order_acquire)) {
    int timeout = -1;
    {
      std::lock_guard lock(impl_->post_mutex_);
      if (!impl_->posted_.empty()) {
        timeout = 0;
      }
    }
    if (!impl_->ready_.empty()) {
      timeout = 0;  // somebody was queued while we were resuming last batch
    }
    if (timeout != 0 && !impl_->timers_.empty()) {
      auto now = clock_type::now();
      auto deadline = impl_->timers_.top().deadline;
      if (deadline <= now) {
        timeout = 0;
      } else {
        auto micros = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
        auto millis = (micros + 999) / 1000;
        timeout = static_cast<int>(std::min<long long>(millis, INT_MAX));
      }
    }

    if (!impl_->engine_->wait(timeout)) {
      break;
    }

    auto now = clock_type::now();
    while (!impl_->timers_.empty() && impl_->timers_.top().deadline <= now) {
      timer_entry entry = impl_->timers_.top();
      impl_->timers_.pop();
      auto waiter = impl_->timer_waiters_.find(entry.id);
      if (waiter != impl_->timer_waiters_.end()) {
        push_ready(waiter->second);
        impl_->timer_waiters_.erase(waiter);
      }
    }

    {
      std::lock_guard lock(impl_->post_mutex_);
      for (handle_type handle : impl_->posted_) {
        push_ready(handle);
      }
      impl_->posted_.clear();
    }

    // Move the batch out: a resumed coroutine may push_ready() again (for
    // example by closing a socket), and that belongs to the next iteration.
    std::vector<handle_type> batch;
    batch.swap(impl_->ready_);
    impl_->queued_.clear();

    for (handle_type handle : batch) {
      handle.resume();
    }
  }

  impl_->running_.store(false, std::memory_order_release);
}

void event_loop::stop() {
  impl_->running_.store(false, std::memory_order_release);
  impl_->wakeup_.signal();
}

void event_loop::post(std::coroutine_handle<> handle) {
  {
    std::lock_guard lock(impl_->post_mutex_);
    impl_->posted_.push_back(handle);
  }
  impl_->wakeup_.signal();
}

void event_loop::watch(int fd, unsigned interests, std::coroutine_handle<> handle) {
  impl_->waiters_[fd] = handle;
  impl_->engine_->watch_fd(fd, interests);
}

void event_loop::unwatch(int fd) {
  impl_->waiters_.erase(fd);
  impl_->engine_->forget_fd(fd);
}

timer_id event_loop::schedule(clock::time_point deadline, std::coroutine_handle<> handle) {
  timer_id id = impl_->next_timer_id_++;
  impl_->timers_.push(timer_entry{deadline, id});
  impl_->timer_waiters_[id] = handle;
  return id;
}

void event_loop::cancel_timer(timer_id id) {
  if (impl_->timer_waiters_.erase(id) == 0) {
    return;
  }
  ++impl_->cancelled_timers_;
  if (impl_->cancelled_timers_ >= 64 &&
      impl_->cancelled_timers_ >= impl_->timer_waiters_.size()) {
    impl_->rebuild_timers();
  }
}

std::size_t event_loop::timer_entry_count() const { return impl_->timers_.size(); }

bool event_loop::is_running() const { return impl_->running_.load(std::memory_order_acquire); }

io_engine& event_loop::engine() { return *impl_->engine_; }

const io_engine& event_loop::engine() const { return *impl_->engine_; }

void event_loop::push_ready(std::coroutine_handle<> handle) {
  if (!handle) {
    return;
  }
  if (impl_->queued_.insert(handle.address()).second) {
    impl_->ready_.push_back(handle);
  }
}

std::coroutine_handle<> event_loop::take_waiter(int fd) {
  auto waiter = impl_->waiters_.find(fd);
  if (waiter == impl_->waiters_.end()) {
    return {};
  }
  auto handle = waiter->second;
  impl_->waiters_.erase(waiter);
  return handle;
}

int event_loop::wakeup_fd() const { return impl_->wakeup_.read_fd(); }

void event_loop::drain_wakeup() { impl_->wakeup_.drain(); }

io_wait_awaiter event_loop::readable(int fd, clock::time_point deadline) {
  return io_wait_awaiter{*this, fd, io_read, deadline};
}

io_wait_awaiter event_loop::writable(int fd, clock::time_point deadline) {
  return io_wait_awaiter{*this, fd, io_write, deadline};
}

sleep_awaiter event_loop::sleep_until(clock::time_point deadline) {
  return sleep_awaiter{*this, deadline};
}

io_wait_awaiter::io_wait_awaiter(event_loop& loop, int fd, unsigned interests,
                                 std::chrono::steady_clock::time_point deadline)
    : loop_(loop), fd_(fd), interests_(interests), deadline_(deadline) {}

io_wait_awaiter::~io_wait_awaiter() {
  // Belt and braces: if the waiting frame is destroyed without ever being
  // resumed, the fd registration must not outlive it.
  loop_.unwatch(fd_);
  if (timer_ != 0) {
    loop_.cancel_timer(timer_);
  }
}

bool io_wait_awaiter::await_suspend(std::coroutine_handle<> handle) {
  loop_.watch(fd_, interests_, handle);
  if (deadline_ != std::chrono::steady_clock::time_point::max()) {
    timer_ = loop_.schedule(deadline_, handle);
  }
  return true;
}

bool io_wait_awaiter::await_resume() {
  loop_.unwatch(fd_);
  if (timer_ != 0) {
    loop_.cancel_timer(timer_);
    timer_ = 0;
  }
  return deadline_ == std::chrono::steady_clock::time_point::max() || loop_.now() < deadline_;
}

sleep_awaiter::sleep_awaiter(event_loop& loop, std::chrono::steady_clock::time_point deadline)
    : loop_(loop), deadline_(deadline) {}

sleep_awaiter::~sleep_awaiter() {
  if (timer_ != 0) {
    loop_.cancel_timer(timer_);
  }
}

void sleep_awaiter::await_suspend(std::coroutine_handle<> handle) {
  timer_ = loop_.schedule(deadline_, handle);
}

}  // namespace coro_http
