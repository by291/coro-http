# coro-http

> **用协程把非阻塞 IO 写成同步的样子。**

一个 C++23 的 HTTP/1.1 server demo。它不是为了堆功能：仓库里每一个文件都服务于上面这一句话。目标读者是想通过读代码搞明白「C++ 协程 + 事件驱动网络编程」到底怎么配合工作的人。

```
app/     路由表、handler、模拟的老式同步 SDK
 |
http/    连接会话协程、增量解析、request/response、优雅关闭
 |
net/     事件循环（kqueue / epoll）、fd 就绪、定时器、socket、accept
 |
runtime/ Task<T>、调度器接口、定时器等待、线程池、blocking_call
```

## 快速开始

```bash
# 依赖（manifest 模式，需要 VCPKG_ROOT）
export VCPKG_ROOT=/path/to/vcpkg

cmake --preset dev
cmake --build build/dev

# 跑测试（62 个用例）
ctest --test-dir build/dev --output-on-failure

# 跑服务
./build/dev/coro-http --port 8080
# 或者优化版
cmake --preset release && cmake --build build/release
./build/release/coro-http --port 8080
```

```bash
curl http://127.0.0.1:8080/hello
curl -X POST --data 'hello body' http://127.0.0.1:8080/echo

# 并发验证：慢请求还在睡的时候，其他连接照常返回
curl 'http://127.0.0.1:8080/slow?ms=2000' &
time curl http://127.0.0.1:8080/hello

# 阻塞桥：一个只会阻塞的外部 SDK 被丢给线程池
curl 'http://127.0.0.1:8080/legacy?key=demo'

curl http://127.0.0.1:8080/stats

# Ctrl-C 触发优雅关闭：停止 accept，半关闭存量连接，等它们自己退栈
```

演示端点每个只演示一件事：

| 路由 | 演示内容 |
|---|---|
| `GET /hello` | 最小路径：解析 → 路由 → 响应 |
| `POST /echo` | 请求体读取与 `max_body_bytes` 限制 |
| `GET /slow?ms=N` | 定时器挂起，且不阻塞其他连接 |
| `GET /legacy?key=K` | `blocking_call`：把同步 SDK 桥接到事件循环 |
| `GET /stats` | 连接与请求计数 |

## 实测数字

本机：Apple Silicon（14 核）、macOS 27.2、Apple clang 21、`Release` 构建、loopback。

```bash
ab -n 20000 -c 100 -k http://127.0.0.1:8080/hello
```

| 指标 | 结果 |
|---|---|
| Requests per second | **124,799** |
| Time per request | 0.801 ms |
| Failed requests | 0 |
| Keep-Alive requests | 20000 / 20000 |

微基准（`bench/coro_http_bench`，Release）：

| 基准 | 时间 |
|---|---|
| 解析典型请求头 | 110 ns |
| 解析 24 个头的请求头 | 386 ns |
| 序列化 1 KiB 响应 | 62 ns |

复现：

```bash
./build/release/bench/coro_http_bench --benchmark_min_time=0.3s
./build/release/coro-http --port 8080 &
bench/bench.sh http://127.0.0.1:8080/hello 20000 100
```

## 它做对了什么

- **单线程 reactor**：所有 handler、所有协程都在事件循环线程上跑。唯一的其他线程是 `blocking_call` 的线程池。
- **协程帧的单所有权**：`Task<T>` 惰性启动、单所有者；`final_suspend` 用 symmetric transfer 把控制权直接交给续体，深链 await 在 clang 与 GCC 优化构建下栈用量不随层数增长（有测试）。子帧在 `await_resume` 里即时销毁并清空句柄，避免「销毁 N 层链递归 N 层」。注意 GCC 在非优化或带 sanitizer 的构建里无法消除每跳 64 字节的栈帧，三条深链测试会在那里跳过并写明原因。
- **一次性兴趣**：suspending 时向事件循环注册 fd 兴趣，恢复时撤销；底层后端保持水平触发，既没有边缘触发的「必须读到 EAGAIN」陷阱，也不会重复唤醒。
- **定时器不进后端**：epoll 没有原生定时器，kqueue 有，因此统一成「最小堆 + poll 超时」，两个后端语义一致。取消采用惰性删除，并在死条目超过活条目时重建堆，避免高并发下堆随「速率 × 超时」增长（有测试）。
- **协议策略集中**：Host 必填、Content-Length / Transfer-Encoding 冲突、重复 Content-Length、obs-fold、各类上限（414 / 431 / 413 / 501 / 505）都在 `parse_request_head()` 里，解析器是纯函数，表驱动测试覆盖畸形输入。
- **阻塞有边界**：`blocking_call` 是唯一允许阻塞的出口；池满时立刻返回错误（应用层转 503），事件循环永不等待 worker。`grep blocking_call` 即可审计全部阻塞点。
- **关闭不硬拔电源**：停止 accept → 对所有连接 `shutdown(SHUT_RD)` → 卡在 read 的协程以 EOF 自然唤醒并退栈 → 宽限期后兜底关 socket。整个过程不会手动 resume 别人的协程帧。

## 阅读顺序

建议按这个顺序读，每一站都有对应文档：

1. `src/runtime/task.h` — 协程帧的生命周期与 symmetric transfer（[docs/coroutines.md](docs/coroutines.md)）
2. `src/net/event_loop.h/.cpp` + `src/net/kqueue_backend.cpp` — 事件循环如何把就绪翻译成 resume
3. `src/runtime/blocking_call.h` + `src/runtime/thread_pool.*` — 跨线程 resume 的同步（[docs/blocking-bridge.md](docs/blocking-bridge.md)）
4. `src/http/session.cpp` — 招牌协程：读、路由、写全部顺序书写
5. `src/http/parse_request_head.cpp` — 增量性在哪、策略在哪（[docs/http.md](docs/http.md)）
6. `src/app/router.cpp` — 最后一层，故意做得很薄

整体架构与数据流见 [docs/architecture.md](docs/architecture.md)。

## 测试与检查

```bash
ctest --test-dir build/dev --output-on-failure   # 62 个用例

cmake --preset asan && cmake --build build/asan
./build/asan/tests/coro_http_tests               # 内存/UB

cmake --preset tsan && cmake --build build/tsan
./build/tsan/tests/coro_http_tests               # 数据竞争
```

sanitizer 构建用 `RelWithDebInfo` + `-O1`：比 `-O0` 快得多，栈回溯仍然可用，UBSan 也不会
被过度优化折叠掉。UBSan 打开 `-fno-sanitize-recover=all`，CI 里另外设置
`ASAN_OPTIONS=detect_leaks=1:abort_on_error=1` 与
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，让报告直接让 job 失败，而不是打印完继续。

测试分三层：

- **单元**：`Task` 生命周期与深链（含一条直接测量析构嵌套深度的回归测试）、事件循环（fd 就绪、超时、跨线程 post）、线程池饱和、解析器表驱动、路由与响应序列化。
- **集成**：真起一个服务，用阻塞 socket 当客户端，覆盖 keep-alive、POST、404/405、并发、413、400、优雅关闭。
- **微基准**：解析与序列化，方便以后替换实现时做对照。

## 用库的地方与手写的地方

「脏活」用 vcpkg 引库，「学习对象」自己写：

| 手写 | 引库 |
|---|---|
| `Task` / promise_type / symmetric transfer | `picohttpparser`：请求行与头的字节级切分 |
| 事件循环 + kqueue/epoll 后端 | `Catch2`：测试 |
| 一次性兴趣的 fd 等待、定时器堆 | `Google Benchmark`：微基准 |
| 会话协程、`blocking_call`、优雅关闭 | |

`picohttpparser` 只负责 tokenize；增量缓冲、body 读取、协议策略、错误映射都在 `parse_request_head.cpp` 和 `session.cpp` 里。想亲手写 tokenizer，只换 `parse_request_head()` 一个函数即可。

## 明确不做

TLS、HTTP/2、WebSocket、静态文件、模板、chunked 请求体、多 reactor / SO_REUSEPORT、日志系统、配置系统。每一个都值得单独诚实地做一遍，摞在一起就是功能拼盘。

其中几个取舍：

- **不做 chunked**：请求体只认 `Content-Length`，出现 `Transfer-Encoding` 返回 501。范围收缩换来解析器可以被完整测试。
- **单线程**：容量边界在内核与一次 `poll` 上；多 reactor 是另一个话题，不在这个 demo 里半途而废。
- **IPv4 only**：`acceptor` 用 `inet_pton`，够演示，也少一层 IPv6 细节。

## 已知限制

- **GCC 下深链的每跳栈开销**：clang 与 GCC 优化构建中，对称转移不占栈；GCC `-O0`
  每跳 64 字节，带 sanitizer 时即使 `-O2` 也有 64–71 字节。10 万层在 GCC `-O0` 下
  需要约 6.4MB 栈，因此日常测试用 5 万层（约 3.2MB）；带 sanitizer 的 GCC 构建里
  连 5 万层也会跳过。实测数据见 [docs/coroutines.md](docs/coroutines.md)。
- **挂起链的销毁是递归的**：正常完成时子帧在 `await_resume` 里逐层释放；但一条链
  若在挂起状态下被销毁，帧析构会沿链递归。本项目实际的 await 链只有三层左右。

## 一个 ASan 抓到的真实坑

测试里最初这样写：

```cpp
spawn(loop, [&]() -> task<void> { co_await loop.sleep_for(1h); }());
```

非 ASan 构建一切正常，ASan 直接报 `stack-use-after-scope`。原因：协程 lambda 的帧里存的是闭包的 `this` 指针，而上面这个闭包是临时对象，语句结束就析构了；协程稍后被唤醒时读的是悬空指针。

规则：**协程 lambda 必须放在具名局部变量里**，保证它活得比协程长。`src/runtime/spawn.h` 的注释里也写了这一条。

## 平台与验证

| 平台 | 后端 | 编译器 | 结果 |
|---|---|---|---|
| macOS 27.2（Apple Silicon） | kqueue | Apple clang 21 | `dev` / `ci` / `asan` / `tsan` 四套构建各 62/62 |
| Ubuntu 26.04（独立 Lima VM） | epoll | GCC 15.2 | Release 62/62；Debug 62/62（跳过 2 条栈测量）；ASan 62/62（跳过 3 条深链测试） |

- 要求 C++23（协程 + `std::expected`）；Linux 侧 GCC 13+ / Clang **19+**。
  clang 18 把 `__cpp_concepts` 报成 `201907L`，而 libstdc++ 用它来决定是否暴露
  `<expected>`，因此 clang 18 + libstdc++ 上 `std::expected` 根本不可见。
- 构建：CMake ≥ 3.25 + vcpkg manifest，presets：`dev` / `release` / `ci` / `asan` / `tsan`。
- CI 同时跑 GCC 与 clang（Ubuntu 上两个编译器各一套，macOS 用 Apple clang），另有两个
  ASan/UBSan job（GCC 与 clang 各一）。clang 的 sanitizer 构建会真正执行全部深链测试；
  GCC 的那份按已知限制跳过它们。
- Linux 验证用的虚拟机是独立实例（`coro-http`），没有动本机已有的环境；
  `limactl start coro-http` 可再次启动。
