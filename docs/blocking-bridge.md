# 阻塞桥：blocking_call 与线程池

## 为什么要它

事件循环的前提是「循环线程上没有任何阻塞调用」。但现实中总有一些 API 拿不到 fd：
老式 SDK、某些数据库驱动、系统调用里的 `getaddrinfo`、只会 sleep 的第三方库。

不能把它们改成异步，也不能让循环线程等它们。唯一正确的做法是：
**把调用挪到别的线程，把结果送回循环线程，让协程在原地恢复。**

## 调用约定

```cpp
auto result = co_await blocking_call(app.pool(), app.loop(), [key] {
  return legacy::fetch(key);        // 同步、阻塞、无法异步化
});
if (!result) {
  // 池已满：立刻返回，事件循环没有等待任何 worker
}
```

- 这是全仓库**唯一**允许出现阻塞调用的入口。审查时 `grep blocking_call` 就能列出全部阻塞点。
- 返回 `std::expected<R, std::error_code>`；饱和时错误码是
  `resource_unavailable_try_again`，应用层转成 `503`。
- 用法边界：只有确实无法异步化的调用才放进来。能用 fd 等待完成的事情，一律走 `co_await read/write/…`。

## 跨线程协议

worker 线程**绝不触碰协程帧**。它只做三件事：

```cpp
result_type produced = std::invoke(std::move(*job_holder));

std::coroutine_handle<> waiter;
{
  std::lock_guard lock(state->mutex);
  state->result.emplace(std::move(produced));
  waiter = std::exchange(state->waiter, {});
}
if (waiter) {
  state->sched->post(waiter);      // 交回循环线程
}
```

循环线程侧：

```cpp
bool await_ready()  { lock; return state->result.has_value(); }
bool await_suspend(handle h) {
  lock;
  if (state->result) return false;   // worker 已经抢先完成
  state->waiter = h;
  return true;
}
```

需要同时防住两个方向：

1. **worker 先完成**：`await_ready` 看到结果就不挂起；如果恰好发生在
   `await_ready` 与 `await_suspend` 之间，`await_suspend` 再查一次并返回 `false`。
2. **协程先挂起**：worker 在锁内取走 `waiter`，锁外 `post`。
   协程帧只会在循环线程上被恢复，因此同一时刻只有一个线程碰它。

`state` 由协程帧与 worker 共享（`shared_ptr`），所以即使 worker 还在跑，
结果槽的生命周期也不依赖协程帧。

## 线程池的边界

```cpp
bool try_submit(std::function<void()> job);   // 满则返回 false，绝不阻塞
```

- `pending()` 同时统计排队与正在运行的任务；
- 达到 `worker_capacity` 后 `try_submit` 立刻失败，事件循环拿到错误后返回 503；
- `stop()` 先排空队列再 join。

「满了就拒绝」是刻意的：如果池饱和时让循环线程等待，阻塞就从 worker 泄漏回了事件循环，
这个 demo 最想演示的规则就破了。

## 关闭时的顺序

```
shutdown() 停止 accept，半关闭所有连接的读端
   |
   +-- 卡在 blocking_call 的协程：等 worker 返回后自行恢复
   |      （shutdown 不会手动 resume 它，因为结果还没写进槽）
   |
   +-- 其它协程：以 EOF / 定时器路径醒来，检查 closing_ 后退出
   |
等待 sessions_ 清空（宽限期）
   |
loop.stop()  ->  run() 里 pool_.stop()：排空队列并 join
```

规则：**不要在阻塞调用还没完成时销毁协程帧**。本仓库通过两点保证：

1. 关闭时先让会话按正常路径退出，不手动恢复帧；
2. 即使还有在途调用，`thread_pool::stop()` 也会等它们跑完再 join。

代价是：如果某个同步调用永远不返回，关闭也会一直等。所以放进 `blocking_call`
的调用必须是有界的——demo 里的 `legacy_sdk::fetch()` 固定睡 50 ms。

## 一个容易忽略的细节

`blocking_call` 把 callable 放进 `shared_ptr` 再交给 `std::function`。
这样即使 callable 是 move-only（比如捕获了 `unique_ptr`），也能塞进线程池队列。
C++23 的 `std::move_only_function` 在部分标准库实现里还没有，所以没有依赖它。
