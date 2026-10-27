# HTTP 层：增量解析、协议策略、连接循环

## 边界

`http/` 只做三件事：

1. 把字节流变成 `request`；
2. 把 `response` 变成字节流；
3. 管理一条连接上多个请求的生命周期。

它不做路由（`app/`）、不做 IO（`net/`）、不碰时间（超时由传入的 deadline 表达）。

## 解析器：库做切分，我们做策略

`picohttpparser` 负责请求行与头的字节级 tokenize，返回指向缓冲区的指针。
留下的是真正有内容的部分：

- **增量判定**：返回 `-2` 表示头还没收完，`session` 继续 `co_await` 读；
- **请求体**：解析器只给头，body 按 `Content-Length` 由 session 自己读满；
- **协议策略**：方法以外的所有校验（见下表）；
- **错误映射**：库只给「成功/不完整/失败」，具体回 400 还是 431 由我们决定。

`parse_request_head()` 是纯函数：输入缓冲区、输出三态（`need_more / complete / invalid`）。
不碰 socket、不碰时钟，所以表驱动测试可以喂任意畸形输入。

## 策略表

| 检查 | 结果 |
|---|---|
| 请求行超过 `max_request_line` | `414 URI Too Long` |
| 头块超过 `max_header_bytes`，或头数量超过 `max_headers` | `431 Request Header Fields Too Large` |
| `Content-Length` 超过 `max_body_bytes` | `413 Payload Too Large` |
| HTTP/1.1 缺少 `Host` | `400 Bad Request` |
| `Transfer-Encoding` 单独出现 | `501 Not Implemented`（本 demo 不支持 chunked） |
| `Transfer-Encoding` 与 `Content-Length` 同时出现 | `400`（防请求走私） |
| 多个 `Content-Length` 且值不同 | `400` |
| `Content-Length` 非数字 / 为负 / 溢出 | `400` |
| 废弃的折行头（obs-fold） | `400` |
| 版本不是 `HTTP/1.0` / `HTTP/1.1` | `505 HTTP Version Not Supported` |
| 其余语法错误 | `400` |

方法策略**不在解析器里**：`DELETE /existing` 会正常解析，由路由发现「路径存在但方法不允许」，
返回 `405` 并带 `Allow` 头。这是 HTTP 语义该有的分工。

## 连接循环

```cpp
while (!closing_) {
  auto [req, keep_alive] = co_await read_request();
  auto res = co_await routes_.dispatch(std::move(req));
  co_await write_all(serialize_response(res, keep_alive, is_head));
  if (!keep_alive) break;
}
```

- **缓冲**：每个连接一个 `std::string`；已消费部分在下一次解析前 `erase` 掉。
  头块上限 32 KiB、body 上限默认为 1 MiB，缓冲区不会无限增长。
- **重解析**：每次收到新字节就把整个待解析头块重新解析一遍。
  代价被头块上限限制在常数范围内，换来的是实现简单、不保存解析中间态。
- **keep-alive**：HTTP/1.1 默认保持，HTTP/1.0 默认关闭；`Connection: close/keep-alive` 覆盖默认值。
- **HEAD**：路由复用 GET handler，序列化时保留 `Content-Length` 但丢弃 body。
- **流水线**：客户端一次发来两个请求时，第二个请求留在缓冲区里，下一轮循环直接解析。

## 超时

| 场景 | 预算 |
|---|---|
| 空闲等待下一个请求的第一个字节 | `keep_alive_timeout`（默认 30 s） |
| 已经收到部分头，等待收完 | `header_timeout`（默认 5 s） |
| 读请求体 | `body_timeout`（默认 10 s） |
| 写响应 | `write_timeout`（默认 10 s） |

超时的实现见 [coroutines.md](coroutines.md)：fd 兴趣与一个定时器同时登记，谁先到谁唤醒。

## 响应序列化

`serialize_response()` 负责帧定界，handler 不需要思考这些：

- 状态行 + 用户设的头；
- 没有显式 `Content-Length` 时自动补（按 body 大小）；
- `Connection` 由帧层决定，handler 设了也会被覆盖；
- HEAD 请求只输出头。

## 背压

`write_all()` 在 `EAGAIN` 时挂起等待可写，而不是把响应塞进无界队列。
因此慢客户端不会让服务端内存增长——代价是这个连接的协程会一直挂在写操作上，
直到写出、超时或连接断开。

## 为什么不用 llhttp

llhttp 是完整的增量状态机，回调式 API。它能力更强，但恰好把「请求是分批到达的、
解析必须能中断再续」这件事藏进了库内部。这个 demo 的学习目标之一就是那件事，
所以选择了「只切分、不管状态」的 picohttpparser，把状态机留在自己的代码里。
