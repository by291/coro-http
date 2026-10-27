#include <sys/event.h>
#include <unistd.h>

#include <cerrno>
#include <unordered_map>
#include <vector>

#include "net/poller.h"

namespace coro_http {

namespace {

class kqueue_poller final : public poller {
public:
  kqueue_poller() : kq_(::kqueue()) {}

  ~kqueue_poller() override {
    if (kq_ >= 0) {
      ::close(kq_);
    }
  }

  bool watch(int fd, unsigned interests) override {
    unsigned& registered = interests_[fd];
    bool ok = apply(fd, EVFILT_READ, (registered & io_read) != 0, (interests & io_read) != 0);
    ok = apply(fd, EVFILT_WRITE, (registered & io_write) != 0, (interests & io_write) != 0) && ok;
    registered = interests;
    return ok;
  }

  void unwatch(int fd) override {
    auto it = interests_.find(fd);
    if (it == interests_.end()) {
      return;
    }
    if (it->second & io_read) {
      apply(fd, EVFILT_READ, true, false);
    }
    if (it->second & io_write) {
      apply(fd, EVFILT_WRITE, true, false);
    }
    interests_.erase(it);
  }

  int wait(io_event* events, int max_events, int timeout_ms) override {
    std::vector<struct kevent> raw(static_cast<std::size_t>(max_events));
    struct timespec timeout{};
    struct timespec* timeout_ptr = nullptr;
    if (timeout_ms >= 0) {
      timeout.tv_sec = timeout_ms / 1000;
      timeout.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1'000'000L;
      timeout_ptr = &timeout;
    }

    int ready = ::kevent(kq_, nullptr, 0, raw.data(), max_events, timeout_ptr);
    if (ready <= 0) {
      return ready;
    }

    for (int i = 0; i < ready; ++i) {
      unsigned flags = 0;
      if (raw[i].filter == EVFILT_READ) {
        flags |= io_event_readable;
      }
      if (raw[i].filter == EVFILT_WRITE) {
        flags |= io_event_writable;
      }
      if (raw[i].flags & EV_EOF) {
        flags |= io_event_hangup;
      }
      if (raw[i].flags & EV_ERROR) {
        flags |= io_event_error;
      }
      events[i] = io_event{static_cast<int>(raw[i].ident), flags};
    }
    return ready;
  }

private:
  bool apply(int fd, int16_t filter, bool had, bool want) {
    if (had == want) {
      return true;
    }
    struct kevent change{};
    EV_SET(&change, static_cast<uintptr_t>(fd), filter, want ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0,
           nullptr);
    return ::kevent(kq_, &change, 1, nullptr, 0, nullptr) == 0;
  }

  int kq_{-1};
  std::unordered_map<int, unsigned> interests_;
};

}  // namespace

std::unique_ptr<poller> make_poller() { return std::make_unique<kqueue_poller>(); }

}  // namespace coro_http
