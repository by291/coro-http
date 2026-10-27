# 协程运行时：Task、挂起、恢复

## 三个问题

读 `runtime/` 时只盯三件事：

1. 协程帧在哪、归谁、什么时候销毁？
2. `co_await` 一个 IO 时，控制权交给了谁？谁把它接回来？
3. 深层嵌套时调用栈会不会涨？

## Task 的不变量

```cpp
template <class T>
class [[nodiscard]] task {
public:
  auto operator co_await() && noexcept;   // 只能被 await 一次
  ~task() { if (handle_) handle_.destroy(); }
private:
  std::coroutine_handle<promise_type> handle_;
};
```

- **惰性启动**：`initial_suspend()` 返回 `suspend_always`。创建一个 task 不会执行任何代码，
  直到有人 `co_await` 它。测试 `a task does not run until it is awaited` 盯着这一点。
- **单所有权**：没有引用计数、没有拷贝。`operator co_await() &&` 把句柄从 task 移进 awaiter，
  task 变空；同一个 task 无法被 await 两次。
- **帧的销毁点**：awaiter 是 `co_await` 表达式里的临时对象，全表达式结束时析构，
  在析构里销毁协程帧。因此 `co_await step()` 的返回值必须先被移动到调用者那边
  （`await_resume()` 返回 `T`，不是 `T&&`）。

## 挂起与恢复

```cpp
struct awaiter {
  bool await_ready() { return false; }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) {
    handle.promise().continuation = continuation;   // 记住谁在等我
    return handle;                                  // 对称转移进 task
  }
  T await_resume() { return std::move(*handle.promise().result); }
};
```

`await_suspend` 返回一个句柄时，运行时会**尾调用**它，而不是在 `resume()` 的栈上继续。
这就是 symmetric transfer：

- `co_await inner()` 不会在栈上留下 `outer` 的帧；
- `inner` 完成时 `final_suspend` 返回 `continuation`，直接跳回 `outer`；
- 没有续体时返回 `std::noop_coroutine()`，控制权回到最初的 `resume()` 调用者。

`recursive_chain(50'000)` 那个测试就是为了证明这一点：没有对称转移，五万层嵌套必然爆栈。

一个实测差异：GCC 在 `-O0` 下每条对称转移边仍会留下几十字节栈帧（`-O2` 下完全消除），
clang 在 `-O0` 下也不留。因此测试取 5 万层——足以证明机制，又能在两个编译器的
Debug 构建下稳定通过。`-O2` 下十万层也没有问题。

## IO 等待是如何接进事件循环的

`event_loop::readable(fd, deadline)` 返回一个 awaiter：

```cpp
bool await_suspend(handle h) {
  loop_.watch(fd_, io_read, h);                       // fd -> 句柄
  if (deadline_ != max) timer_ = loop_.schedule(deadline_, h);
  return true;
}
bool await_resume() {
  loop_.unwatch(fd_);                                 // 一次性兴趣
  if (timer_) loop_.cancel_timer(timer_);
  return deadline_ == max || loop_.now() < deadline_; // 超时没？
}
```

事件循环的 `run()` 每轮做四件事：

1. `poll`（kqueue/epoll；超时来自最近一个定时器）；
2. 把就绪 fd 的句柄取出，放进 `ready`；
3. 把到期定时器的句柄取出，放进 `ready`；
4. 去重后依次 `resume()`。

去重很重要：一个句柄可能同时登记了 fd 和定时器。同一批里两个来源都触发时，
`queued` 集合保证它只被恢复一次；恢复后 `await_resume` 会撤销另一个登记。

## 一次性兴趣 vs 水平触发

后端是水平触发（`poll` 一直报告可读，直到读空）。但上层看到的是「一次挂起、一次唤醒」：

- 挂起时登记兴趣；
- 恢复时立刻撤销。

好处是既不用面对边缘触发「必须读到 EAGAIN 为止，否则再也不会被唤醒」的陷阱，
也不会出现同一个等待被反复唤醒。

## 超时

fd 等待可以把定时器和 fd 登记在一起：

```cpp
bool ready = co_await loop_.readable(fd, deadline);
```

- fd 先就绪 → 恢复，`await_resume` 取消定时器；
- 定时器先到期 → 恢复，`await_resume` 撤销 fd 兴趣；
- 返回 `false` 表示截止时间已过。

`socket::read_some()` 拿到 `false` 后还会再试一次 `recv`：数据可能在截止瞬间到达，
不该因为「醒来晚了一微秒」就判超时。

## 启动一个顶层协程：spawn

`spawn(scheduler&, task<void>, OnDone)` 用一个「分离的驱动协程」跑任务：

```cpp
auto driver = [](task<void> work, OnDone on_done) -> detail::detached_coro {
  co_await std::move(work);   // work 的帧在这里结束
  on_done();                  // 之后才调用回调
}(std::move(work), std::move(on_done));
sched.post(driver.handle);
```

`detached_coro` 的 `final_suspend` 是 `suspend_never`，帧自己销毁。
回调在任务帧销毁**之后**执行，所以它是放掉「最后一个所有者」的安全位置
——`server::accept_loop()` 正是用它把 `session` 从注册表里摘掉。

## 同步驱动：sync_wait

只在测试和微基准里用。它构造一个驱动协程，`resume()` 一次，
对称转移链跑完整个任务后回到调用者。它不能跨越真正需要事件循环的 await。

## 必须记住的坑

**协程 lambda 不能是临时对象。**

```cpp
spawn(loop, [&]() -> task<void> { ... }());   // 错
auto body = [&]() -> task<void> { ... };
spawn(loop, body());                          // 对
```

协程帧里存的是闭包的 `this`。临时闭包在语句结束时就析构了，
而协程可能还挂起在 IO 上；醒来时通过悬空 `this` 读捕获变量。
普通构建很可能「碰巧正常」，ASan 会报 `stack-use-after-scope`。

## 错误模型

core 层不用异常传递错误：`std::error_code` / `std::expected<T, std::error_code>`。
`unhandled_exception()` 一律 `std::terminate()`——抛异常穿过协程帧被视为 bug。
对端关闭、超时、解析失败这些服务端常态，都是值，不是异常。
