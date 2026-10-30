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

测试 `awaiting a deep chain does not grow the call stack` 直接比较 500 层与 5000 层
在链最深处读到的栈地址：对称转移成立时两者相同，不成立时差值随层数增长。

## 帧的销毁同样是设计的一部分

单所有权意味着「A 的帧里存着拥有 B 帧的 awaiter」。子帧的销毁点有两种选择：

- 放在 awaiter 的析构函数里 —— 如果编译器把 `co_await` 临时对象保留到帧析构
  （GCC 在非优化、以及带 sanitizer 的构建里就是这样），销毁一条 N 层链会递归 N 层；
- 放在 `await_resume()` 里（本项目的做法）—— 子帧已经停在 `final_suspend`，
  取走返回值后立即销毁并清空句柄，父帧里就只剩下空 awaiter。

```cpp
T value = std::move(*handle.promise().result);
release_frame();
return value;
```

析构函数仍保留一份兜底，用于「链还挂着就被销毁」的情况：那条路径确实是递归的，
所以**可能被取消的 await 链要保持浅**（本项目实际的链只有 session -> dispatch ->
handler 三层左右）。

## 深链与编译器：一个实测结论

测试 `awaiting a deep chain does not grow the call stack` 比较 500 层与 5000 层
在链最深处读到的栈地址。下面是**恢复路径**的每跳栈用量：

| 编译器 / 构建 | 每跳栈用量 |
|---|---|
| clang（含 ASan / TSan，`-O0`） | 0 字节 |
| GCC `-O2` / `-O3`，无 sanitizer | 0 字节 |
| GCC `-O0` | 64 字节 |
| GCC + ASan（任何优化级别） | 64–71 字节 |

也就是说，GCC 只在「优化开启且没有 sanitizer 插桩」时才真正实现对称转移的无栈增长。
10 万层在 GCC `-O0` 下需要 6.4MB 栈，会爆；在 clang 或 GCC 优化构建下没问题。

这不是本项目的实现问题（`await_suspend` 返回下一个句柄就是标准要求的对称转移写法），
而是 GCC 的代码生成特性。

测试因此这样安排：

- 深链完成测试取 **5 万层**：GCC `-O0` 下约 3.2MB 栈，能过；10 万层需要约 6.4MB，会爆，
  所以不作为日常测试深度（这个结论保留在本文档里）。带 sanitizer 的 GCC 构建里
  连 5 万层都放不下，该测试跳过；clang 的 sanitizer 构建能过。
- 两套「直接测量栈差」的测试（恢复路径、析构嵌套）在**所有 sanitizer 构建**和
  GCC 非优化构建里会 `SKIP`：插桩会改变栈布局，测量结果不再反映被测行为。
  实测中 Linux clang 19 + ASan 的析构测量同样给出每层 64 字节的假信号。

普通构建（`ci` / `release`）与 macOS 的 `dev` 构建会真正执行这两套测量。

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

## 完成引擎：io_uring

就绪模型是「等 fd 可读，然后自己 syscall」；完成模型是「提交操作，等 CQE」。

```cpp
co_await engine.read_some(fd, buffer, deadline);
```

- `await_suspend` 里取一个 SQE、`io_uring_prep_recv`、把 `user_data` 记成操作 id，然后挂起；
- 有 deadline 时再提交一个 `IORING_OP_TIMEOUT`，两个操作共享同一份状态（还剩几个 CQE、
  谁先完成、结果是什么）；
- 第一个完成的操作决定结果并取消另一个（`IORING_OP_ASYNC_CANCEL`）；
  两个 CQE 都收割完之后才 `push_ready` 恢复协程；
- 缓冲区要活到 CQE 到达：它属于挂起的调用方协程帧，天然满足；
- `socket::close()` 先 `cancel(fd)` 再关 fd，否则可能在关闭后还有完成写回旧缓冲区。

这里踩过一个坑：引擎的 `wait()` 收割完 CQE 后如果继续阻塞等下一个事件，
刚刚 `push_ready` 的协程就会一直躺在 ready 队列里没人执行。规则是
**本轮处理过完成就立即返回**，把控制权交回事件循环。

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
