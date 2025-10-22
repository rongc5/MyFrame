# MyFrame 异步响应与定时器快速参考

**适用读者**: 需要在 MyFrame 中实现异步响应或定时任务的业务开发者  
**最后更新时间**: 2025-10-22

---

## 立即上手

```cpp
class MyAsyncProcess : public http_base_data_process {
private:
    enum : uint32_t { TIMER_ASYNC = 1 };
    std::string _body;

public:
    void msg_recv_finish() override {
        set_async_response_pending(true);              // 1. 标记为异步

        auto timer = std::make_shared<timer_msg>();    // 2. 构造定时器
        timer->_timer_type  = TIMER_ASYNC;             //    自定义类型
        timer->_time_length = 1000;                    //    1 秒后触发
        timer->_obj_id      = get_base_net()->get_id()._id; //    绑定连接
        add_timer(timer);                              // 3. 注册定时器
    }

    void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override {
        if (!t_msg || t_msg->_timer_type != TIMER_ASYNC) {
            return;
        }

        auto& res = _base_process->get_res_head_para();
        res._response_code = 200;
        res._response_str  = "OK";
        res._headers["Content-Type"] = "text/plain";
        _body = "async done";
        res._headers["Content-Length"] = std::to_string(_body.size());

        complete_async_response();                     // 4. 结束异步流程
    }

    std::string* get_send_body(int& result) override {
        result = 1;
        if (_body.empty()) return nullptr;
        auto* out = new std::string(_body);
        _body.clear();
        return out;
    }
};
```

---

## `timer_msg` 关键字段

| 字段 | 说明 | 必填 | 典型值 |
| ---- | ---- | ---- | ------ |
| `_obj_id` | 对应的连接 ID，不设置会导致触发失败 | ✅ | `get_base_net()->get_id()._id` |
| `_timer_type` | 自定义类型，用于区分不同定时器 | ✅ | `enum class TimerType { Async = 1, Timeout = 2 };` |
| `_time_length` | 延迟毫秒数 | ✅ | `1000` (1 秒), `5000` (5 秒) |
| `_timer_id` | 系统自动写入，无需手动赋值 | ❌ | - |

---

## 常用延迟

```
100    // 100 ms
1000   // 1 秒
5000   // 5 秒
30000  // 30 秒
60000  // 1 分钟
```

---

## 常见定时器声明方式

```cpp
// 常量
constexpr uint32_t TIMER_ASYNC   = 1;
constexpr uint32_t TIMER_TIMEOUT = 2;

// 枚举
enum class TimerType : uint32_t {
    Async   = 1,
    Timeout = 2,
    Heartbeat = 3
};
```

---

## 典型流程回顾

1. `msg_recv_finish()` 收到请求
2. 调用 `set_async_response_pending(true)`
3. 初始化 `timer_msg` 并完成 `_obj_id`、`_timer_type`、`_time_length`
4. `add_timer(timer)` 提交定时器
5. 框架线程调度到 `handle_timeout()`
6. 构建响应并调用 `complete_async_response()`
7. 业务逻辑返回，框架发送响应

---

## 双定时器模式（业务 + 超时）

```cpp
void msg_recv_finish() override {
    set_async_response_pending(true);

    auto async_timer = std::make_shared<timer_msg>();
    async_timer->_timer_type  = TIMER_ASYNC;
    async_timer->_time_length = 1000;
    async_timer->_obj_id      = get_base_net()->get_id()._id;
    add_timer(async_timer);

    auto timeout_timer = std::make_shared<timer_msg>();
    timeout_timer->_timer_type  = TIMER_TIMEOUT;
    timeout_timer->_time_length = 5000;
    timeout_timer->_obj_id      = async_timer->_obj_id;
    add_timer(timeout_timer);
}

void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override {
    if (_completed.exchange(true)) {
        return; // 只处理第一次触发
    }

    auto& res = _base_process->get_res_head_para();

    if (t_msg->_timer_type == TIMER_ASYNC) {
        build_success_response(res);
    } else {
        build_timeout_response(res);
    }

    complete_async_response();
}
```

---

## Level 1 / Level 2 支持情况

- **Level 1 (`IApplicationHandler`)**: 默认同步；如需异步或定时器，请切换到 Level 2 或 Level 3。
- **Level 2 (`HttpContext`)**: 使用 `ctx.async_response()` 与 `ctx.complete_async_response()`；定时器依旧通过 `add_timer()` 完成。
- **Level 3 (`base_data_process`)**: 完全控制字节流；需自行维护异步状态以及定时器类型。

---

## 常见错误排查

| 现象 | 可能原因 | 解决办法 |
| ---- | -------- | -------- |
| `handle_timeout()` 未触发 | `_obj_id` 未赋值或值错误 | 调用 `get_base_net()->get_id()._id` 赋值 |
| 响应未发送 | 忘记调用 `complete_async_response()` | 在所有分支调用完成方法 |
| 定时器重复触发 | 没有幂等保护 | 在业务中增加状态标记或移除定时器 |
| 框架提示 “async pending” | 忘记回填响应或清理状态 | 检查所有异常路径是否调用完成方法 |

---

## 相关文档

- `docs/TIMER_USAGE_GUIDE.md` —— 定时器详细指南
- `docs/ASYNC_RESPONSE_GUIDE.md` —— 异步响应完整说明
- `docs/ASYNC_AND_TIMER_COMPLETE.md` —— 组合能力总结

---

**温馨提示**：务必在单元测试或示例中验证响应路径的所有分支（成功、失败、超时），防止连接被悬挂。
