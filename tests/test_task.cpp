#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include "runtime/sync_wait.h"
#include "runtime/task.h"

using coro_http::sync_wait;
using coro_http::task;

namespace {

task<int> add_one(int value) { co_return value + 1; }

// Writes out the address of a real stack slot. Returning it as an integer
// trips -Wreturn-stack-address, hence the out parameter.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void stack_marker(std::uintptr_t& out) {
  volatile char local = 0;
  out = reinterpret_cast<std::uintptr_t>(&local);
}

task<int> measured_chain(int depth, std::uintptr_t* deepest) {
  if (depth == 0) {
    stack_marker(*deepest);
    co_return 0;
  }
  co_return 1 + co_await measured_chain(depth - 1, deepest);
}

// Records the stack address of every frame teardown. If destroying a chain
// recurses, the destructors are nested and the addresses spread far apart;
// if each frame is released as its parent resumes, they stay clustered.
struct teardown_probe {
  static inline bool started = false;
  static inline std::uintptr_t first = 0;
  static inline std::uintptr_t last = 0;

  static void reset() {
    started = false;
    first = 0;
    last = 0;
  }

  ~teardown_probe() {
    std::uintptr_t here = 0;
    stack_marker(here);
    if (!started) {
      started = true;
      first = here;
    }
    last = here;
  }
};

task<int> probed_chain(int depth) {
  [[maybe_unused]] teardown_probe probe;
  if (depth == 0) {
    co_return 0;
  }
  co_return 1 + co_await probed_chain(depth - 1);
}

std::uintptr_t run_measured_chain(int depth) {
  std::uintptr_t deepest = 0;
  REQUIRE(sync_wait(measured_chain(depth, &deepest)) == depth);
  return deepest;
}

task<int> recursive_chain(int depth) {
  if (depth == 0) {
    co_return 0;
  }
  co_return 1 + co_await recursive_chain(depth - 1);
}

task<std::unique_ptr<int>> make_unique_value(int value) {
  co_return std::make_unique<int>(value);
}

}  // namespace

TEST_CASE("a task does not run until it is awaited") {
  bool ran = false;
  auto body = [&]() -> task<int> {
    ran = true;
    co_return 1;
  };

  auto work = body();
  REQUIRE_FALSE(ran);

  REQUIRE(sync_wait(std::move(work)) == 1);
  REQUIRE(ran);
}

TEST_CASE("a task passes its return value to the awaiter") {
  REQUIRE(sync_wait(add_one(41)) == 42);
}

TEST_CASE("a task can return a move-only value") {
  auto value = sync_wait(make_unique_value(7));
  REQUIRE(value);
  REQUIRE(*value == 7);
}

TEST_CASE("a task<void> runs to completion") {
  int steps = 0;
  auto body = [&]() -> task<void> {
    ++steps;
    co_return;
  };

  sync_wait(body());
  REQUIRE(steps == 1);
}

TEST_CASE("nested tasks return the innermost value") {
  auto outer = []() -> task<int> { co_return co_await add_one(1) + 1; };
  REQUIRE(sync_wait(outer()) == 3);
}

TEST_CASE("destroying an unawaited task destroys its frame") {
  int live = 0;

  // A coroutine that has never been resumed has no body locals yet, so the
  // probe has to live in a parameter: that copy is part of the frame. Counting
  // live instances tolerates the transient copy made while calling in.
  struct lifetime_probe {
    int* live;
    explicit lifetime_probe(int* counter) : live(counter) { ++*live; }
    lifetime_probe(const lifetime_probe& other) : live(other.live) { ++*live; }
    lifetime_probe(lifetime_probe&& other) noexcept : live(other.live) { ++*live; }
    ~lifetime_probe() { --*live; }
  };

  auto body = [](lifetime_probe probe) -> task<void> {
    (void)probe;
    co_return;
  };

  lifetime_probe probe{&live};
  REQUIRE(live == 1);
  {
    auto work = body(probe);
    REQUIRE(live == 2);  // one in the frame, one in this scope
  }

  REQUIRE(live == 1);  // the frame's copy is gone
}

// GCC only elides the per-hop stack frame of symmetric transfer in optimised,
// uninstrumented builds: 64 bytes per hop at -O0, and sanitizer
// instrumentation keeps it even at -O2. The tests below that need many hops
// therefore skip there, with the reason attached.
#if defined(__GNUC__) && !defined(__clang__) &&                                   \
    (!defined(__OPTIMIZE__) || defined(__SANITIZE_ADDRESS__) ||                   \
     defined(__SANITIZE_THREAD__))
#define CORO_HTTP_GCC_MANY_HOPS_UNSAFE 1
#endif

TEST_CASE("completing a deep chain does not nest frame destruction") {
#if defined(CORO_HTTP_GCC_MANY_HOPS_UNSAFE)
  // On these builds the resumption frames themselves nest, which drowns out
  // any teardown nesting this measures.
  SKIP("GCC keeps a stack frame per symmetric-transfer hop outside optimised, uninstrumented builds");
#else
  teardown_probe::reset();
  REQUIRE(sync_wait(probed_chain(10'000)) == 10'000);

  const std::uintptr_t growth = teardown_probe::last > teardown_probe::first
                                    ? teardown_probe::last - teardown_probe::first
                                    : teardown_probe::first - teardown_probe::last;
  REQUIRE(growth < 64 * 1024);
#endif
}

TEST_CASE("completing a 100k-deep chain does not overflow the stack") {
#if defined(CORO_HTTP_GCC_MANY_HOPS_UNSAFE)
  SKIP("GCC keeps a stack frame per symmetric-transfer hop outside optimised, uninstrumented builds");
#else
  REQUIRE(sync_wait(recursive_chain(100'000)) == 100'000);
#endif
}

TEST_CASE("awaiting a deep chain does not grow the call stack") {
#if defined(CORO_HTTP_GCC_MANY_HOPS_UNSAFE)
  SKIP("GCC keeps a stack frame per symmetric-transfer hop outside optimised, uninstrumented builds");
#else
  // Compare where the stack sits at the deepest point of a short chain and a
  // much longer one. With symmetric transfer both hops land at the same
  // address; without it, each level adds a resume frame and the difference
  // grows with the depth.
  const std::uintptr_t shallow = run_measured_chain(500);
  const std::uintptr_t deep = run_measured_chain(5'000);

  const std::uintptr_t growth = deep > shallow ? deep - shallow : shallow - deep;
  REQUIRE(growth < 64 * 1024);
#endif
}
