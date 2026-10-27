#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "net/event_loop.h"
#include "runtime/blocking_call.h"
#include "runtime/spawn.h"
#include "runtime/task.h"
#include "runtime/thread_pool.h"

using namespace std::chrono_literals;

using coro_http::blocking_call;
using coro_http::event_loop;
using coro_http::spawn;
using coro_http::task;
using coro_http::thread_pool;

TEST_CASE("the pool runs submitted work on a worker thread") {
  thread_pool pool(2);
  std::thread::id caller = std::this_thread::get_id();
  std::thread::id worker{};
  std::mutex mutex;
  std::condition_variable done;
  bool finished = false;

  REQUIRE(pool.try_submit([&] {
    {
      std::lock_guard lock(mutex);
      worker = std::this_thread::get_id();
      finished = true;
    }
    done.notify_one();
  }));

  std::unique_lock lock(mutex);
  done.wait(lock, [&] { return finished; });

  REQUIRE(worker != caller);

  // pending_ is decremented after the job returns, which happens just after
  // the job notifies, so give the worker a moment to finish its bookkeeping.
  for (int i = 0; i < 1000 && pool.pending() != 0; ++i) {
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE(pool.pending() == 0);
  pool.stop();
}

TEST_CASE("the pool rejects work once it is at capacity") {
  thread_pool pool(1, 1);
  std::atomic<bool> release{false};

  REQUIRE(pool.try_submit([&] {
    while (!release.load()) {
      std::this_thread::yield();
    }
  }));
  REQUIRE_FALSE(pool.try_submit([] {}));

  release.store(true);
  pool.stop();
}

TEST_CASE("stop drains queued work before joining") {
  thread_pool pool(1, 16);
  std::atomic<int> completed{0};

  for (int i = 0; i < 8; ++i) {
    REQUIRE(pool.try_submit([&] { completed.fetch_add(1); }));
  }

  pool.stop();
  REQUIRE(completed.load() == 8);
  REQUIRE(pool.pending() == 0);
}

TEST_CASE("blocking_call resumes the coroutine on the loop thread") {
  event_loop loop;
  thread_pool pool(2);

  std::thread::id caller = std::this_thread::get_id();
  std::thread::id worker{};
  std::thread::id resumed_on{};
  int value = 0;
  bool had_value = false;

  auto body = [&]() -> task<void> {
    auto result = co_await blocking_call(pool, loop, [&] {
      worker = std::this_thread::get_id();
      std::this_thread::sleep_for(5ms);
      return 42;
    });
    resumed_on = std::this_thread::get_id();
    had_value = result.has_value();
    if (result) {
      value = *result;
    }
    loop.stop();
  };
  spawn(loop, body());

  loop.run();

  REQUIRE(had_value);
  REQUIRE(value == 42);
  REQUIRE(worker != caller);
  REQUIRE(resumed_on == caller);
}

TEST_CASE("blocking_call reports a saturated pool instead of blocking the loop") {
  event_loop loop;
  thread_pool pool(1, 1);
  std::atomic<bool> release{false};

  REQUIRE(pool.try_submit([&] {
    while (!release.load()) {
      std::this_thread::yield();
    }
  }));

  bool saturated = false;
  bool loop_still_progressed = false;

  auto body = [&]() -> task<void> {
    auto result = co_await blocking_call(pool, loop, [] { return 1; });
    saturated = !result.has_value();
    loop_still_progressed = true;
    loop.stop();
  };
  spawn(loop, body());

  loop.run();

  release.store(true);
  pool.stop();

  REQUIRE(saturated);
  REQUIRE(loop_still_progressed);
}

TEST_CASE("two blocking calls run concurrently on the pool") {
  event_loop loop;
  thread_pool pool(2);
  std::atomic<int> running{0};
  std::atomic<int> peak{0};
  std::atomic<int> finished{0};

  auto work = [&]() -> task<void> {
    auto result = co_await blocking_call(pool, loop, [&] {
      int now = running.fetch_add(1) + 1;
      int observed = peak.load();
      while (now > observed && !peak.compare_exchange_weak(observed, now)) {
      }
      std::this_thread::sleep_for(30ms);
      running.fetch_sub(1);
      return now;
    });
    (void)result;
    if (finished.fetch_add(1) + 1 == 2) {
      loop.stop();
    }
  };

  spawn(loop, work());
  spawn(loop, work());

  loop.run();
  pool.stop();

  REQUIRE(peak.load() == 2);
}
