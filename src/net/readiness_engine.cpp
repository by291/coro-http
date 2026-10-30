#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <memory>
#include <stdexcept>
#include <span>
#include <string_view>
#include <vector>

#include "net/event_loop.h"
#include "net/io_engine.h"
#include "net/poller.h"

namespace coro_http {

namespace {

constexpr int kMaxEvents = 64;

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool is_would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }

std::error_code current_error() { return std::error_code(errno, std::system_category()); }

// epoll/kqueue: the kernel reports readiness, we do the syscall. The coroutine
// parks on "fd is readable/writable" and retries after every wakeup.
class readiness_engine final : public io_engine {
public:
  explicit readiness_engine(event_loop& loop)
      : loop_(loop), poller_(make_poller()), events_(kMaxEvents) {
    poller_->watch(loop_.wakeup_fd(), io_read);
  }

  [[nodiscard]] const char* name() const override { return "readiness"; }

  bool wait(int timeout_ms) override {
    int count = poller_->wait(events_.data(), kMaxEvents, timeout_ms);
    if (count < 0) {
      return errno == EINTR;  // a signal is not fatal; anything else is
    }
    for (int i = 0; i < count; ++i) {
      const int fd = events_[static_cast<std::size_t>(i)].fd;
      if (fd == loop_.wakeup_fd()) {
        loop_.drain_wakeup();
        continue;
      }
      if (auto handle = loop_.take_waiter(fd)) {
        loop_.push_ready(handle);
      }
    }
    return true;
  }

  task<std::expected<std::size_t, std::error_code>> read_some(
      int fd, std::span<char> buffer, clock::time_point deadline) override {
    for (;;) {
      ssize_t n = ::recv(fd, buffer.data(), buffer.size(), 0);
      if (n >= 0) {
        co_return static_cast<std::size_t>(n);
      }
      if (errno == EINTR) {
        continue;
      }
      if (!is_would_block(errno)) {
        co_return std::unexpected(current_error());
      }

      bool ready = co_await loop_.readable(fd, deadline);
      if (!ready) {
        // The deadline expired, but data can land in the same instant: try the
        // syscall once more before reporting a timeout.
        ssize_t last = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (last >= 0) {
          co_return static_cast<std::size_t>(last);
        }
        if (errno == EINTR) {
          continue;
        }
        if (is_would_block(errno)) {
          co_return std::unexpected(std::make_error_code(std::errc::timed_out));
        }
        co_return std::unexpected(current_error());
      }
    }
  }

  task<std::expected<void, std::error_code>> write_all(int fd, std::string_view data,
                                                       clock::time_point deadline) override {
    while (!data.empty()) {
      ssize_t n = ::send(fd, data.data(), data.size(), kSendFlags);
      if (n > 0) {
        data.remove_prefix(static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n < 0 && is_would_block(errno)) {
        bool ready = co_await loop_.writable(fd, deadline);
        if (!ready) {
          co_return std::unexpected(std::make_error_code(std::errc::timed_out));
        }
        continue;
      }
      co_return std::unexpected(current_error());
    }
    co_return std::expected<void, std::error_code>{};
  }

  task<std::expected<int, std::error_code>> accept(int listener,
                                                   clock::time_point deadline) override {
    for (;;) {
      int client = ::accept(listener, nullptr, nullptr);
      if (client >= 0) {
        co_return client;
      }
      if (errno == EINTR) {
        continue;
      }
      if (is_would_block(errno)) {
        co_await loop_.readable(listener, deadline);
        continue;
      }
      co_return std::unexpected(current_error());
    }
  }

  void cancel(int fd) override {
    forget_fd(fd);
    if (auto handle = loop_.take_waiter(fd)) {
      loop_.push_ready(handle);
    }
  }

  void watch_fd(int fd, unsigned interests) override { poller_->watch(fd, interests); }

  void forget_fd(int fd) override { poller_->unwatch(fd); }

private:
  event_loop& loop_;
  std::unique_ptr<poller> poller_;
  std::vector<io_event> events_;
};

}  // namespace

std::unique_ptr<io_engine> make_io_engine(io_backend backend, event_loop& loop) {
#ifdef CORO_HTTP_HAVE_IO_URING
  if (backend == io_backend::uring || backend == io_backend::auto_select) {
    if (auto engine = make_uring_engine(loop)) {
      return engine;
    }
    if (backend == io_backend::uring) {
      throw std::runtime_error("io_uring is not available on this system");
    }
  }
#else
  if (backend == io_backend::uring) {
    throw std::runtime_error("this build has no io_uring engine");
  }
#endif
  return std::make_unique<readiness_engine>(loop);
}

}  // namespace coro_http
