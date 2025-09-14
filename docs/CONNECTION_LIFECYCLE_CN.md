# 连接生命周期与优雅关闭（客户端/服务端通用）

本文档说明 MyFrame 中连接对象的统一回收路径与推荐关闭方式，帮助业务在不影响线程/其他连接的前提下安全断开连接。

## 回收路径（统一）

- 连接对象生命周期由容器 `common_obj_container` 管理：
  - 对端关闭（`recv=0`）、异常（抛出 `CMyCommonException`）、或超时/业务关闭 → 容器捕获 → `destroy()` → 从容器映射擦除 → 对象析构。
  - `base_net_obj` 析构中会执行 `epoll_ctl(DEL)` 与 `::close(fd)`，确保事件循环与 fd 一致清理。

## 推荐关闭方式（业务侧）

不要直接 `close(fd)`。请使用以下任意一种方式，以便走统一回收路径：

1) 立即关闭（建议在事件回调上下文内调用）
```
// 在 process（如 process_recv_buf / handle_msg / msg_recv_finish）中：
close_now(); // base_data_process::close_now()
```

2) 几乎立即/延迟关闭（例如“发完再关”）
```
request_close_after(1); // base_data_process::request_close_after(ms)
// 或 request_close_now();
```

说明：
- `close_now()` 会抛出异常到事件循环，容器捕获后统一回收；
- `request_close_after(ms)` 通过短定时器触发统一回收；
- 二者均不会停止线程，也不会影响其它连接。

## 典型误区

- 直接 `close(fd)`: 可能导致 epoll 中残留、容器状态不一致；请改用上述 API。
- 单连接完成调用 `stop_all_thread()`: 会停止所有网络线程，不适合多连接服务；已从框架默认路径移除。

## 何时调用关闭 API

- 客户端：
  - 收到完整响应后、或业务判定不再需要保持连接时；
  - 超时/异常时：可在超时回调中调用 `close_now()`（HTTP/2 客户端已改为此方式）。
- 服务端：
  - 发送完业务数据、或检测到错误/越权等场景需要立即断开时。

## 示例（HTTP 客户端优雅关闭）

参见示例 `examples/http_close_demo.cpp`，演示在 `msg_recv_finish()` 中调用 `request_close_after(1)` 优雅关闭连接。

