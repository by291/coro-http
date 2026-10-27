#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "runtime/sync_wait.h"
#include "runtime/task.h"

using coro_http::sync_wait;
using coro_http::task;

namespace {

task<int> add_one(int value) { co_return value + 1; }

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

TEST_CASE("a deep await chain does not grow the call stack") {
  // Without symmetric transfer each nested co_await would add a full resume
  // frame and this depth would overflow. With it, growth per hop is at most a
  // few bytes on GCC's -O0 codegen (and zero with optimisation, or on clang),
  // so 50k hops fit comfortably in the default stack.
  REQUIRE(sync_wait(recursive_chain(50'000)) == 50'000);
}
