#include <liburing.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "net/event_loop.h"
#include "net/io_engine.h"

namespace coro_http {

namespace {

constexpr unsigned kRingEntries = 256;
constexpr std::uint64_t kWakeupTag = 0;
constexpr std::uint64_t kCancelTag = 1;
constexpr std::uint64_t kFirstOpId = 2;

using clock_type = std::chrono::steady_clock;

__kernel_timespec to_timespec(clock_type::time_point deadline) {
  const auto now = clock_type::now();
  const auto remaining = deadline > now ? deadline - now : clock_type::duration::zero();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining - seconds);
  return __kernel_timespec{static_cast<long long>(seconds.count()),
                           static_cast<long long>(nanos.count())};
}

struct completion {
  std::int32_t result{0};
  bool timed_out{false};
};

// One logical operation may need two completions: the I/O itself and a
// timeout racing it. The first completion wins and cancels the other; the
// coroutine resumes once both have been reaped.
struct shared_state {
  std::coroutine_handle<> waiter{};
  completion* sink{nullptr};
  int remaining{0};
  bool done{false};
};

struct operation {
  std::uint64_t id{0};
  std::shared_ptr<shared_state> shared;
  int fd{-1};
  bool is_timeout{false};
  std::uint64_t sibling_id{0};
  __kernel_timespec timeout{};
};

enum class op_kind { recv, send, accept };

class uring_engine final : public io_engine {
public:
  explicit uring_engine(event_loop& loop);
  ~uring_engine() override;

  [[nodiscard]] const char* name() const override { return "io_uring"; }

  bool wait(int timeout_ms) override;

  task<std::expected<std::size_t, std::error_code>> read_some(
      int fd, std::span<char> buffer, clock::time_point deadline) override;

  task<std::expected<void, std::error_code>> write_all(
      int fd, std::string_view data, clock::time_point deadline) override;

  task<std::expected<int, std::error_code>> accept(int listener,
                                                   clock::time_point deadline) override;

  void cancel(int fd) override;

  // A completion engine has no readiness registrations.
  void watch_fd(int, unsigned) override {}
  void forget_fd(int) override {}

  struct awaitable {
    uring_engine& engine;
    op_kind kind{op_kind::recv};
    int fd{-1};
    std::span<char> read_buffer{};
    std::string_view write_data{};
    clock::time_point deadline{clock::time_point::max()};
    completion result{};

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> waiter) {
      return engine.start(*this, waiter);
    }
    completion await_resume() const { return result; }
  };

private:
  bool start(awaitable& op, std::coroutine_handle<> waiter);
  io_uring_sqe* get_sqe();
  void arm_wakeup();
  void submit_cancel(std::uint64_t target_id);
  void handle_cqe(io_uring_cqe* cqe);
  int drain_completions();
  bool submit_pending();

  event_loop& loop_;
  io_uring ring_{};
  bool ring_ready_{false};
  bool wakeup_armed_{false};
  std::uint64_t next_op_id_{kFirstOpId};
  std::unordered_map<std::uint64_t, operation*> live_;
};

uring_engine::uring_engine(event_loop& loop) : loop_(loop) {
  int rc = io_uring_queue_init(kRingEntries, &ring_, 0);
  if (rc < 0) {
    throw std::system_error(-rc, std::system_category(), "io_uring_queue_init");
  }
  ring_ready_ = true;
  arm_wakeup();
}

uring_engine::~uring_engine() {
  for (auto& [id, op] : live_) {
    (void)id;
    delete op;
  }
  live_.clear();
  if (ring_ready_) {
    io_uring_queue_exit(&ring_);
  }
}

io_uring_sqe* uring_engine::get_sqe() {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (auto* sqe = io_uring_get_sqe(&ring_)) {
      return sqe;
    }
    if (io_uring_submit(&ring_) < 0) {
      return nullptr;
    }
  }
  return nullptr;
}

void uring_engine::arm_wakeup() {
  if (wakeup_armed_) {
    return;
  }
  auto* sqe = get_sqe();
  if (sqe == nullptr) {
    return;
  }
  io_uring_prep_poll_add(sqe, loop_.wakeup_fd(), POLLIN);
  io_uring_sqe_set_data64(sqe, kWakeupTag);
  wakeup_armed_ = true;
}

void uring_engine::submit_cancel(std::uint64_t target_id) {
  if (auto* sqe = get_sqe()) {
    io_uring_prep_cancel64(sqe, target_id, 0);
    io_uring_sqe_set_data64(sqe, kCancelTag);
  }
}

bool uring_engine::start(awaitable& op, std::coroutine_handle<> waiter) {
  auto shared = std::make_shared<shared_state>();
  shared->waiter = waiter;
  shared->sink = &op.result;

  auto* primary = new operation{next_op_id_++, shared, op.fd, false, 0, {}};
  io_uring_sqe* sqe = get_sqe();
  if (sqe == nullptr) {
    delete primary;
    op.result.result = -ENOMEM;
    return false;  // resume immediately with the error
  }

  switch (op.kind) {
    case op_kind::recv:
      io_uring_prep_recv(sqe, op.fd, op.read_buffer.data(), op.read_buffer.size(), 0);
      break;
    case op_kind::send:
      io_uring_prep_send(sqe, op.fd, op.write_data.data(), op.write_data.size(), 0);
      break;
    case op_kind::accept:
      io_uring_prep_accept(sqe, op.fd, nullptr, nullptr, 0);
      break;
  }
  io_uring_sqe_set_data64(sqe, primary->id);
  live_.emplace(primary->id, primary);
  ++shared->remaining;

  if (op.deadline != clock::time_point::max()) {
    auto* timer = new operation{next_op_id_++, shared, op.fd, true, 0, to_timespec(op.deadline)};
    if (auto* timeout_sqe = get_sqe()) {
      io_uring_prep_timeout(timeout_sqe, &timer->timeout, 0, 0);
      io_uring_sqe_set_data64(timeout_sqe, timer->id);
      live_.emplace(timer->id, timer);
      ++shared->remaining;
      primary->sibling_id = timer->id;
      timer->sibling_id = primary->id;
    } else {
      delete timer;
    }
  }

  return true;
}

void uring_engine::handle_cqe(io_uring_cqe* cqe) {
  const std::uint64_t tag = cqe->user_data;
  if (tag == kWakeupTag) {
    loop_.drain_wakeup();
    wakeup_armed_ = false;
    arm_wakeup();
    return;
  }
  if (tag == kCancelTag) {
    return;  // the cancel request's own completion
  }

  auto found = live_.find(tag);
  if (found == live_.end()) {
    return;  // stale completion for an operation we already retired
  }
  operation* op = found->second;
  live_.erase(found);

  auto shared = op->shared;
  if (!shared->done) {
    shared->done = true;
    shared->sink->result = cqe->res;
    shared->sink->timed_out = op->is_timeout;
    if (op->sibling_id != 0) {
      submit_cancel(op->sibling_id);
    }
  }
  delete op;

  if (--shared->remaining == 0) {
    loop_.push_ready(shared->waiter);
  }
}

int uring_engine::drain_completions() {
  int count = 0;
  io_uring_cqe* cqe = nullptr;
  while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
    handle_cqe(cqe);
    io_uring_cqe_seen(&ring_, cqe);
    ++count;
  }
  return count;
}

bool uring_engine::submit_pending() { return io_uring_submit(&ring_) >= 0; }

bool uring_engine::wait(int timeout_ms) {
  arm_wakeup();
  if (!submit_pending()) {
    return false;
  }

  // Handing completions to the loop means there is work to resume; do not
  // block again waiting for an unrelated event.
  if (drain_completions() > 0 || timeout_ms == 0) {
    return true;
  }

  io_uring_cqe* cqe = nullptr;
  int rc = 0;
  if (timeout_ms < 0) {
    rc = io_uring_wait_cqe(&ring_, &cqe);
  } else {
    __kernel_timespec ts{timeout_ms / 1000, (timeout_ms % 1000) * 1'000'000LL};
    rc = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
  }
  if (rc == -EINTR || rc == -ETIME) {
    return true;
  }
  if (rc < 0) {
    return false;
  }

  handle_cqe(cqe);
  io_uring_cqe_seen(&ring_, cqe);
  drain_completions();
  return true;
}

task<std::expected<std::size_t, std::error_code>> uring_engine::read_some(
    int fd, std::span<char> buffer, clock::time_point deadline) {
  awaitable op{.engine = *this,
               .kind = op_kind::recv,
               .fd = fd,
               .read_buffer = buffer,
               .deadline = deadline};
  completion done = co_await op;
  if (done.timed_out) {
    co_return std::unexpected(std::make_error_code(std::errc::timed_out));
  }
  if (done.result < 0) {
    co_return std::unexpected(std::error_code(-done.result, std::system_category()));
  }
  co_return static_cast<std::size_t>(done.result);
}

task<std::expected<void, std::error_code>> uring_engine::write_all(
    int fd, std::string_view data, clock::time_point deadline) {
  while (!data.empty()) {
    awaitable op{.engine = *this,
                 .kind = op_kind::send,
                 .fd = fd,
                 .write_data = data,
                 .deadline = deadline};
    completion done = co_await op;
    if (done.timed_out) {
      co_return std::unexpected(std::make_error_code(std::errc::timed_out));
    }
    if (done.result <= 0) {
      if (done.result == 0) {
        co_return std::unexpected(std::make_error_code(std::errc::broken_pipe));
      }
      co_return std::unexpected(std::error_code(-done.result, std::system_category()));
    }
    data.remove_prefix(static_cast<std::size_t>(done.result));
  }
  co_return std::expected<void, std::error_code>{};
}

task<std::expected<int, std::error_code>> uring_engine::accept(int listener,
                                                              clock::time_point deadline) {
  awaitable op{.engine = *this,
               .kind = op_kind::accept,
               .fd = listener,
               .deadline = deadline};
  completion done = co_await op;
  if (done.timed_out) {
    co_return std::unexpected(std::make_error_code(std::errc::timed_out));
  }
  if (done.result < 0) {
    co_return std::unexpected(std::error_code(-done.result, std::system_category()));
  }
  co_return done.result;
}

void uring_engine::cancel(int fd) {
  // Cancel the primary operation; its completion will cancel the sibling
  // timeout. Ids remain stable even if the operation is retired meanwhile.
  std::vector<std::uint64_t> targets;
  for (const auto& [id, op] : live_) {
    if (op->fd == fd && !op->is_timeout) {
      targets.push_back(id);
    }
  }
  for (std::uint64_t id : targets) {
    submit_cancel(id);
  }
}

}  // namespace

std::unique_ptr<io_engine> make_uring_engine(event_loop& loop) {
  try {
    return std::make_unique<uring_engine>(loop);
  } catch (const std::system_error&) {
    return nullptr;  // old kernel, seccomp, no memory: let auto_select fall back
  }
}

}  // namespace coro_http
