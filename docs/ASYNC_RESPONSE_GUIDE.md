# MyFrame 异步响应实践指南

**文档版本**: 1.0  
**最后更新时间**: 2025-10-22

---

## 1. 概述

在 MyFrame 中，异步响应用于解决以下场景：

- 业务需要等待耗时的 I/O（数据库、远程服务、文件处理等）。
- 请求需要交给内部队列或后台线程处理，完成后再返回结果。
- 需要在不阻塞网络线程的情况下执行复杂逻辑。

框架支持在不同抽象层实现异步：

| 层级 | 接口 | 特点 |
| ---- | ---- | ---- |
| Level 1 | `IApplicationHandler` | 默认同步，适合简单业务。需要异步时建议升级到 Level 2。 |
| Level 2 | `HttpContext` / `WsContext` 等 | 提供 `async_response()` 与 `complete_async_response()` 辅助方法。 |
| Level 3 | `base_data_process` | 最灵活，直接操作字节流，通常配合定时器实现。 |

---

## 2. Level 2 核心 API

```cpp
class HttpContext {
public:
    virtual const HttpRequest& request() const = 0;
    virtual HttpResponse& response() = 0;

    // 在网络线程之外执行 fn，执行完成后继续发送响应
    virtual void async_response(std::function<void()> fn) = 0;

    // 通知框架异步流程已经结束
    virtual void complete_async_response() = 0;
};
```

使用建议：

1. 在请求入口调用 `ctx.async_response()`，将耗时逻辑放入后台执行。
2. 在后台逻辑的结尾调用 `ctx.complete_async_response()`，框架会发送响应。
3. 如需与定时器配合，可在 `async_response` 内注册定时器，或在 `handle_timeout()` 中调用 `ctx.complete_async_response()`。

---

## 3. Level 3 核心步骤

```cpp
void http_base_data_process::msg_recv_finish();       // 收到完整请求
void http_base_data_process::set_async_response_pending(bool);
void http_base_data_process::complete_async_response();
```

典型流程：

1. 请求到达后调用 `set_async_response_pending(true)` 阻止框架立即发送响应。
2. 启动后台任务或注册定时器。
3. 在任务完成或定时器回调内构建响应数据。
4. 调用 `complete_async_response()` 结束流程。

---

## 4. 基础示例（Level 2）

```cpp
class UserHandler : public IProtocolHandler {
public:
    void on_http_request(HttpContext& ctx) override {
        if (ctx.request().url != "/api/users") {
            return;
        }

        ctx.async_response([&ctx]() {
            auto users = query_users_from_db();
            ctx.response().set_json(serialize(users));
            ctx.complete_async_response();
        });
    }
};
```

---

## 5. 常见模式

### 5.1 线程池或任务队列

```cpp
ctx.async_response([this, &ctx]() {
    auto result = _thread_pool.submit(&Service::do_heavy_job, _service, ctx.request());
    ctx.response().set_json(result.get());
    ctx.complete_async_response();
});
```

### 5.2 消息队列回调

```cpp
void QueueHandler::on_http_request(HttpContext& ctx) {
    uint64_t rid = ctx.connection_info().connection_id;
    _pending[rid] = &ctx;

    send_to_queue(rid, ctx.request());     // 投递任务
}

void QueueHandler::handle_msg(std::shared_ptr<normal_msg>& msg) {
    auto rid = extract_request_id(msg);
    auto it = _pending.find(rid);
    if (it == _pending.end()) return;

    HttpContext* ctx = it->second;
    ctx->response().set_json(extract_payload(msg));
    ctx->complete_async_response();
    _pending.erase(it);
}
```

### 5.3 异步 + 定时器保护

```cpp
set_async_response_pending(true);
_schedule_async_job();

auto timeout = std::make_shared<timer_msg>();
timeout->_timer_type  = kTimeoutGuard;
timeout->_time_length = 5000;
timeout->_obj_id      = get_base_net()->get_id()._id;
add_timer(timeout);
```

在 `handle_timeout()` 中返回 504，并调用 `complete_async_response()` 防止连接悬挂。

---

## 6. 错误处理与清理

- **幂等性**：确保 `complete_async_response()` 只调用一次，可利用 `std::atomic_flag` 或状态枚举。
- **异常捕获**：后台任务内捕获异常，记录日志并返回错误响应，避免线程崩溃。
- **资源管理**：在析构函数或超时路径清理 `_pending` 映射，防止内存泄漏。
- **链路超时**：超时定时器触发后必须回填响应，否则连接会长时间保持挂起状态。

---

## 7. 调试与测试

- 在开发环境打开详细日志，确认请求进入/离开异步流程的顺序。
- 单元测试可将定时器时间缩短，并模拟成功、失败、超时三种分支。
- 使用 `curl -v` 或 `wrk` 压测时观察响应时间是否与预期一致。
- 多线程场景建议使用 TSAN/Valgrind 检查潜在数据竞争。

---

## 8. 关联文档

- `docs/ASYNC_TIMER_QUICK_REF.md` —— 快速参考卡片
- `docs/TIMER_USAGE_GUIDE.md` —— 定时器细节
- `docs/ASYNC_AND_TIMER_COMPLETE.md` —— 能力整合概览

