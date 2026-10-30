#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#include <vector>

#include "net/event_loop.h"
#include "runtime/current_handle.h"
#include "runtime/spawn.h"

using namespace std::chrono_literals;

using coro_http::event_loop;
using coro_http::spawn;
using coro_http::task;

namespace {

void set_nonblocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  REQUIRE(flags >= 0);
  REQUIRE(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

struct pipe_pair {
  int read_fd{-1};
  int write_fd{-1};

  pipe_pair() {
    int fds[2] = {-1, -1};
    REQUIRE(::pipe(fds) == 0);
    read_fd = fds[0];
    write_fd = fds[1];
    set_nonblocking(read_fd);
    set_nonblocking(write_fd);
  }

  ~pipe_pair() {
    if (read_fd >= 0) ::close(read_fd);
    if (write_fd >= 0) ::close(write_fd);
  }

  pipe_pair(const pipe_pair&) = delete;
  pipe_pair& operator=(const pipe_pair&) = delete;
};

}  // namespace

// Note: every coroutine body below is a *named* local. A coroutine captured by
// a temporary lambda keeps a pointer to that closure in its frame, so the
// closure has to outlive the coroutine.

TEST_CASE("readable wait resumes when data is already available") {
  pipe_pair pipe;
  event_loop loop(coro_http::io_backend::readiness);
  bool resumed = false;

  auto body = [&]() -> task<void> {
    co_await loop.readable(pipe.read_fd);
    resumed = true;
    loop.stop();
  };
  spawn(loop, body());

  REQUIRE(::write(pipe.write_fd, "x", 1) == 1);
  loop.run();

  REQUIRE(resumed);
}

TEST_CASE("a fd wait times out without any traffic") {
  pipe_pair pipe;
  event_loop loop(coro_http::io_backend::readiness);
  bool ready = true;
  std::chrono::steady_clock::duration elapsed{};

  auto body = [&]() -> task<void> {
    auto start = std::chrono::steady_clock::now();
    ready = co_await loop.readable(pipe.read_fd, start + 20ms);
    elapsed = std::chrono::steady_clock::now() - start;
    loop.stop();
  };
  spawn(loop, body());

  loop.run();

  REQUIRE_FALSE(ready);
  REQUIRE(elapsed >= 20ms);
}

TEST_CASE("sleep_for resumes after the deadline") {
  event_loop loop(coro_http::io_backend::readiness);
  std::chrono::steady_clock::duration elapsed{};

  auto body = [&]() -> task<void> {
    auto start = std::chrono::steady_clock::now();
    co_await loop.sleep_for(20ms);
    elapsed = std::chrono::steady_clock::now() - start;
    loop.stop();
  };
  spawn(loop, body());

  loop.run();

  REQUIRE(elapsed >= 20ms);
}

TEST_CASE("timers fire in deadline order") {
  event_loop loop(coro_http::io_backend::readiness);
  std::vector<int> order;

  auto later = [&]() -> task<void> {
    co_await loop.sleep_for(30ms);
    order.push_back(2);
    loop.stop();
  };
  auto sooner = [&]() -> task<void> {
    co_await loop.sleep_for(5ms);
    order.push_back(1);
  };

  spawn(loop, later());
  spawn(loop, sooner());

  loop.run();

  REQUIRE(order == std::vector<int>{1, 2});
}

TEST_CASE("post from another thread wakes the loop") {
  event_loop loop(coro_http::io_backend::readiness);
  bool resumed_early = false;

  auto body = [&]() -> task<void> {
    auto self = co_await coro_http::current_handle();
    std::thread poster([&loop, self]() {
      std::this_thread::sleep_for(20ms);
      loop.post(self);
    });

    // An hour would never elapse during the test: only the post can bring
    // this coroutine back.
    co_await loop.sleep_for(1h);
    resumed_early = true;
    loop.stop();
    poster.join();
  };
  spawn(loop, body());

  auto start = std::chrono::steady_clock::now();
  loop.run();
  auto elapsed = std::chrono::steady_clock::now() - start;

  REQUIRE(resumed_early);
  REQUIRE(elapsed < 1s);
}

TEST_CASE("cancelled timers do not pile up in the heap") {
  event_loop loop(coro_http::io_backend::readiness);

  std::vector<coro_http::timer_id> ids;
  ids.reserve(1000);
  for (int i = 0; i < 1000; ++i) {
    ids.push_back(loop.schedule(loop.now() + 1h, {}));
  }
  for (coro_http::timer_id id : ids) {
    loop.cancel_timer(id);
  }

  // Without compaction the heap would still hold all 1000 dead entries until
  // their deadlines arrived.
  REQUIRE(loop.timer_entry_count() <= 128);
}

TEST_CASE("stop makes run return") {
  event_loop loop(coro_http::io_backend::readiness);
  bool ran = false;

  auto body = [&]() -> task<void> {
    ran = true;
    loop.stop();
    co_return;
  };
  spawn(loop, body());

  loop.run();
  REQUIRE(ran);
}
