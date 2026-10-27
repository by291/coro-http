#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <unordered_map>
#include <vector>

#include "net/poller.h"

namespace coro_http {

namespace {

// Linux has no kqueue: a dynamic vector is the idiomatic stand-in for a fixed
// upper bound on the number of fds multiplexed by one epoll instance.
constexpr int kMaxEventsPerWait = 64;
constexpr int kInitialEventBuffer = 64;

class epoll_poller final : public poller {
public:
  epoll_poller() : epfd_(::epoll_create1(EPOLL_CLOEXEC)) {
    events_.resize(kInitialEventBuffer);
  }

  ~epoll_poller() override {
    if (epfd_ >= 0) {
      ::close(epfd_);
    }
  }

  bool watch(int fd, unsigned interests) override {
    struct epoll_event event{};
    event.events = 0;
    if (interests & io_read) {
      event.events |= EPOLLIN;
    }
    if (interests & io_write) {
      event.events |= EPOLLOUT;
    }
    event.events |= EPOLLRDHUP;
    event.data.fd = fd;

    auto it = registered_.find(fd);
    int op = it == registered_.end() ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    if (::epoll_ctl(epfd_, op, fd, &event) != 0) {
      return false;
    }
    registered_[fd] = event.events;
    return true;
  }

  void unwatch(int fd) override {
    if (registered_.erase(fd) != 0) {
      ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    }
  }

  int wait(io_event* events, int max_events, int timeout_ms) override {
    int capacity = std::min(max_events, kMaxEventsPerWait);
    if (static_cast<std::size_t>(capacity) > events_.size()) {
      events_.resize(static_cast<std::size_t>(capacity));
    }
    int ready = ::epoll_wait(epfd_, events_.data(), capacity, timeout_ms);
    if (ready <= 0) {
      return ready;
    }
    for (int i = 0; i < ready; ++i) {
      unsigned flags = 0;
      const auto mask = events_[i].events;
      if (mask & EPOLLIN) {
        flags |= io_event_readable;
      }
      if (mask & EPOLLOUT) {
        flags |= io_event_writable;
      }
      if (mask & EPOLLERR) {
        flags |= io_event_error;
      }
      if (mask & (EPOLLHUP | EPOLLRDHUP)) {
        flags |= io_event_hangup;
      }
      events[i] = io_event{events_[i].data.fd, flags};
    }
    return ready;
  }

private:
  int epfd_{-1};
  std::unordered_map<int, uint32_t> registered_;
  std::vector<struct epoll_event> events_;
};

}  // namespace

std::unique_ptr<poller> make_poller() { return std::make_unique<epoll_poller>(); }

}  // namespace coro_http
