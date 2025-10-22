# MyFrame 定时器使用指南

**文档版本**: 1.0  
**最后更新时间**: 2025-10-22

---

## 1. 背景

MyFrame 在传输层提供统一的定时器能力，用于实现异步响应、心跳保活、超时保护、周期任务等。定时器由网络工作线程调度，触发时回调到业务进程对象（`base_data_process` 派生类）的 `handle_timeout()` 接口。

---

## 2. 定时器生命周期

```
构造 timer_msg → add_timer() → 框架注册 → 到期触发 → handle_timeout() → 业务处理完成
```

- **构造阶段**：业务代码创建 `std::shared_ptr<timer_msg>`，填写关键字段。
- **注册阶段**：调用 `add_timer()` 将定时器提交给当前连接所属的网络线程。
- **触发阶段**：到期后，网络线程唤醒对应的 `base_data_process::handle_timeout()`。
- **业务阶段**：在 `handle_timeout()` 中执行逻辑，可选择再次注册新的定时器。

> 当前版本暂不提供显式的取消 API。若需要“取消”，请通过内部状态忽略后续触发或设置极小的 `_time_length` 立即触发并在处理函数内判断。

---

## 3. `timer_msg` 字段说明

| 字段 | 类型 | 说明 |
| ---- | ---- | ---- |
| `_obj_id` | `uint32_t` | 绑定的连接 ID，**必须设置**。可通过 `get_base_net()->get_id()._id` 获得。 |
| `_timer_type` | `uint32_t` | 自定义类型，用于区分不同逻辑。建议统一管理常量或枚举。 |
| `_timer_id` | `uint32_t` | 由框架在 `add_timer()` 期间写入，表示内部 ID。业务无需赋值。 |
| `_time_length` | `uint32_t` | 延迟时间，单位毫秒。|

示例：

```cpp
auto timer = std::make_shared<timer_msg>();
timer->_timer_type  = kAsyncTimer;
timer->_time_length = 3000;                 // 3 秒
if (auto conn = get_base_net()) {
    timer->_obj_id = conn->get_id()._id;    // 必须设置
}
add_timer(timer);
```

---

## 4. 创建与触发

### 4.1 在请求入口处注册

```cpp
void MyProcess::msg_recv_finish() {
    set_async_response_pending(true);

    auto timer = std::make_shared<timer_msg>();
    timer->_timer_type  = kAsyncTimer;
    timer->_time_length = 1000;
    timer->_obj_id      = get_base_net()->get_id()._id;

    add_timer(timer);
}
```

### 4.2 在 `handle_timeout()` 中处理

```cpp
void MyProcess::handle_timeout(std::shared_ptr<timer_msg>& t_msg) {
    if (!t_msg) return;

    switch (t_msg->_timer_type) {
    case kAsyncTimer:
        on_async_ready();
        break;
    case kTimeoutGuard:
        on_request_timeout();
        break;
    default:
        PDEBUG("unknown timer type: %u", t_msg->_timer_type);
        break;
    }
}
```

---

## 5. 管理多个定时器

### 5.1 定义统一的类型枚举

```cpp
enum class TimerType : uint32_t {
    Async          = 1,
    TimeoutGuard   = 2,
    Heartbeat      = 3,
    PeriodicFlush  = 4,
};
```

### 5.2 在对象内追踪状态

```cpp
class StatefulProcess : public http_base_data_process {
    std::atomic<bool> _async_done {false};

public:
    void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override {
        if (!t_msg || _async_done.load()) {
            return;
        }

        if (t_msg->_timer_type == static_cast<uint32_t>(TimerType::Async)) {
            complete_success();
            _async_done.store(true);
        } else if (t_msg->_timer_type == static_cast<uint32_t>(TimerType::TimeoutGuard)) {
            send_timeout_response();
            _async_done.store(true);
        }

        if (_async_done.load()) {
            complete_async_response();
        }
    }
};
```

---

## 6. 常见模式

### 6.1 异步任务 + 超时保护

1. 请求到达时注册两个定时器：业务完成定时器、超时定时器。
2. 在 `handle_timeout()` 中使用状态位保证只处理一次。
3. 成功路径调用业务逻辑并结束异步；超时路径返回错误码并结束异步。

### 6.2 周期任务 / 心跳

```cpp
void HeartbeatProcess::handle_timeout(std::shared_ptr<timer_msg>& t_msg) {
    if (!t_msg) return;

    if (t_msg->_timer_type == kHeartbeat) {
        send_heartbeat_frame();

        auto next = std::make_shared<timer_msg>(*t_msg); // 复制参数
        next->_timer_id = 0;                              // 必须清零，框架会重新分配
        add_timer(next);
    }
}
```

### 6.3 延时重试

在失败路径中再次注册定时器，并记录重试次数：

```cpp
if (_retry_count < kMaxRetry) {
    ++_retry_count;
    auto retry = std::make_shared<timer_msg>();
    retry->_timer_type  = kRetry;
    retry->_time_length = 200;
    retry->_obj_id      = get_base_net()->get_id()._id;
    add_timer(retry);
} else {
    complete_with_error();
}
```

---

## 7. 调试技巧

- 打开 `PDEBUG` 或自定义日志，输出 `_timer_type`、`_time_length`、`_timer_id`。
- 若定时器未触发，优先检查 `_obj_id` 是否为零。
- 合理设置日志前缀，区分同一连接上的多个定时器。
- 单元测试中可将 `_time_length` 调整为较小数值，加速验证。

---

## 8. 常见问题

| 问题 | 说明 | 处理建议 |
| ---- | ---- | -------- |
| `add_timer` 返回失败 | `_time_length` 为 0 或 `_obj_id` 未设置 | 检查字段并重新提交 |
| 定时器重复触发 | 没有重置 `_timer_id` | 复制 `timer_msg` 后记得将 `_timer_id` 置 0 |
| 超时后仍旧等待 | 忘记调用 `complete_async_response()` | 在所有异常路径结束异步 |
| 频繁触发导致阻塞 | `handle_timeout()` 内执行耗时阻塞操作 | 将耗时逻辑放入线程池或重新注册异步任务 |

---

## 9. 参考资料

- `docs/ASYNC_RESPONSE_GUIDE.md` —— 异步响应协作流程
- `docs/ASYNC_TIMER_QUICK_REF.md` —— 快速上手与常见陷阱
- `examples/simple_async_test.cpp` —— 官方异步 + 定时器示例

***EOF***
