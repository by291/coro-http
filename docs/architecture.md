# 架构与一次请求的完整旅程

## 分层

```
app/       router（表驱动 + :param）、handlers、legacy_sdk
             |  只依赖 http/ 的 Request/Response 与 Task
http/      session（连接协程）、parse_request_head、response、server
             |  依赖 net/ 的 socket/event_loop，依赖 runtime/ 的 Task
net/       io_engine（readiness | io_uring）、event_loop（定时器、ready 批次）、
           poller 接口、socket、acceptor、wakeup_pipe
             |  event_loop 实现 runtime/ 的 scheduler 接口
runtime/   Task<T>、promise、scheduler、spawn、sync_wait、async_event、thread_pool、blocking_call
```

依赖方向单向向下。唯一的倒置点是 `scheduler`：`runtime/` 只声明「怎么把一个协程句柄排回循环线程」，由 `net/event_loop` 实现，因此 `runtime/` 不知道 `net/` 的存在。

## IO 引擎

`io_engine` 是事件循环与内核之间的缝。上层只看到四个操作：

```cpp
co_await engine.read_some(fd, buffer, deadline);
co_await engine.write_all(fd, data, deadline);
co_await engine.accept(listener, deadline);
engine.cancel(fd);   // 唤醒挂在 fd 上的协程，然后才允许关闭 fd
```

每次循环迭代还会调用一次 `engine.wait(timeout_ms)`：引擎负责阻塞，并通过
`event_loop::push_ready()` 把可以恢复的协程交回来。`watch_fd/forget_fd` 只有就绪引擎用得到，
io_uring 的实现是空的。

| 引擎 | 做法 |
|---|---|
| readiness | 非阻塞 syscall；`EAGAIN` 时在事件循环登记一次性的 fd 兴趣，醒来重试 |
| io_uring | 提交操作；CQE 到达时恢复协程。带 deadline 的 I/O 还会提交一个 timeout SQE，先完成的一方取消另一方 |

超时与取消是完成引擎里最绕的部分：一次逻辑读可能有两个在途操作（I/O 与定时器），
协程必须在两个 CQE 都收割完之后才恢复，否则内核可能还在往调用方的缓冲区里写。
`socket::close()` 先 `cancel(fd)` 再 `::close(fd)`，保证挂起的协程带着错误醒来。

## 线程模型

| 线程 | 数量 | 职责 |
|---|---|---|
| 事件循环线程 | 1 | 所有 socket IO、定时器、handler、协程 |
| 线程池 | `workers`（默认 2） | 只跑 `blocking_call` 提交的同步调用 |
| 外部线程 | 0 | 测试/信号只通过 `post()` 或 `async_event` 进入循环 |

线程池线程**不接触协程帧**。它们只写结果槽，然后通过 `scheduler::post()` 把句柄交回循环线程。

## 一次 GET /hello 的旅程

1. `event_loop::run()` 在 `poll` 里等待；监听 fd 就绪。
2. `accept_loop()` 协程被 resume，`co_await acceptor_.accept()` 返回新 fd。
3. 创建 `session`，`spawn()` 把它的 `run()` 协程排到循环上。
4. `session::read_request()`：
   - 缓冲区为空，`co_await socket_.read_some()`；
   - readiness 引擎：`recv` 返回 `EAGAIN`，向事件循环登记一次性的可读兴趣；
     io_uring 引擎：提交一个读操作（一个 SQE）；
   - 协程挂起（帧停在 `await_suspend` 处）。
5. 数据到达：`poll` 报告可读，或 CQE 落进完成队列；事件循环取出对应的句柄并 `resume()`。
6. `read_some` 重试 `recv`，拿到字节；`parse_request_head()` 返回 `complete`。
7. `co_await routes_.dispatch(std::move(req))`：路由协程匹配到 handler，
   对称转移进 handler；handler `co_return` 一个 response，对称转移回来。
8. `serialize_response()` 补上 `Content-Length` / `Connection`，
   `co_await socket_.write_all()`；全部写出后回到循环。
9. keep-alive 为真时回到第 4 步；否则关闭 socket，会话帧销毁，
   `spawn` 的完成回调从 `sessions_` 里摘掉它。

整个过程中，第 4 步到第 5 步之间发生在别的连接上的所有事件，都在这同一个线程上交错执行。

## 所有权与生命周期

| 对象 | 谁拥有 | 何时销毁 |
|---|---|---|
| `Task<T>` 的协程帧 | `task` 对象（单所有权） | `co_await` 全表达式结束时 |
| `session` 协程帧 | `spawn()` 的 driver 帧 | 会话协程完成时 |
| `session` 对象 | `spawn()` 完成回调里的 `shared_ptr` | 帧销毁之后（回调里） |
| socket fd | `stream_socket` | `close()` 或析构 |
| 定时器 | `event_loop` 的最小堆 | 触发或被取消 |

`session` 的注册表只存裸指针，仅用于关闭时遍历；真正的所有者是完成回调里的 `shared_ptr`。这样避免了「协程帧还在跑、对象已经被析构」的经典自毁。

## 关闭顺序

```
SIGINT/SIGTERM
  -> 信号处理器只往 self-pipe 写一个字节
  -> 循环线程上的 watch_for_signal 协程醒来 -> request_stop()
  -> async_event 唤醒 watch_for_stop -> shutdown()
      1. acceptor.close()          停止接受新连接
      2. 对每个 session: shutdown(SHUT_RD)
            卡在 read 的协程以 EOF 自然唤醒，看到 closing_ 后退出
      3. 宽限期内等待 sessions_ 变空
      4. 仍未退出：force_close() 关闭 socket
      5. loop.stop()
  -> run() 返回，销毁 accept 协程帧，线程池 stop()（排空后 join）
```

关键点：**关闭路径从不手动 resume 别人的协程帧**。手动 resume 会和「fd 刚好就绪」的事件竞争，导致同一个帧被唤醒两次。半关闭读端让等待自己按正常路径醒来，唯一需要接管的只有 accept 协程的帧（循环停了之后由 `run()` 显式销毁）。
