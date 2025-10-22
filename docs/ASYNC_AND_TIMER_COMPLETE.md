# MyFrame 异步响应与定时器能力总览

**文档版本**: 1.0  
**最后更新时间**: 2025-10-22

---

## 1. 设计初衷

异步响应与定时器是 MyFrame 处理复杂业务的核心能力：

- **异步响应**：避免在网络线程上阻塞，支持后台任务、消息队列等模式。
- **定时器**：提供延迟执行、超时保护、周期执行、心跳维持等能力。
- **两者结合**：通过状态管理与定时器保证异步流程最终能返回结果或错误。

---

## 2. 核心构件

| 能力 | 入口 | 说明 |
| ---- | ---- | ---- |
| 异步标记 | `http_base_data_process::set_async_response_pending(true)` | Level 3 显式声明异步流程。 |
| 异步回调 | `HttpContext::async_response()` | Level 2 在后台线程执行耗时逻辑。 |
| 完成通知 | `complete_async_response()` | Level 2/3 通知框架发送响应。 |
| 定时器注册 | `add_timer(std::shared_ptr<timer_msg>)` | 任意层级注册定时器。 |
| 定时器回调 | `handle_timeout(std::shared_ptr<timer_msg>&)` | 业务处理到期逻辑。 |

---

## 3. 层级关系

```
Level 1 (IApplicationHandler)
    └─ 适合同步或极轻量异步，推荐向 Level 2 迁移
Level 2 (Context / Handler)
    └─ 提供 async_response + complete_async_response
Level 3 (base_data_process)
    └─ 可直接控制异步状态、定时器、字节流
```

> 文档所述能力完全向下兼容旧版 `base_data_process` 风格，已有代码无需强制改写。

---

## 4. 整体流程示意

```
┌──────────────┐          ┌────────────────┐          ┌────────────────┐
│ 请求到达     │          │ 业务决定异步   │          │ 注册定时器     │
│ msg_recv_finish │ ----> │ set_async_response_pending │ -> add_timer   │
└──────────────┘          └────────────────┘          └────┬───────────┘
                                                             │
                                                             ▼
                                               ┌────────────────────────────┐
                                               │ 后台任务 / 定时器回调     │
                                               │ - 构建响应                │
                                               │ - 可重新注册其他定时器    │
                                               └─────────┬──────────────────┘
                                                         │
                                                         ▼
                                              complete_async_response()
                                                         │
                                                         ▼
                                               框架发送响应并释放资源
```

---

## 5. 常见组合模式

### 5.1 异步任务 + 超时保护

- 注册两个定时器（任务完成、超时兜底），在回调中通过状态确保只响应一次。
- 在任何路径调用 `complete_async_response()`。

### 5.2 周期性推送

- 在心跳回调中重新注册定时器，记得重置 `_timer_id`。
- 若需要停止心跳，设置状态位阻止后续注册。

### 5.3 消息驱动

- 通过 `handle_msg()` 接收后台结果，在消息回调中结束异步。
- 配置定时器作为兜底，防止队列长时间无响应。

---

## 6. 实现要点清单

- [ ] 定时器 `_obj_id` 始终指向当前连接。
- [ ] 异步流程所有分支都调用 `complete_async_response()`。
- [ ] 如有多个定时器，使用枚举统一管理 `_timer_type`。
- [ ] 复制 `timer_msg` 时将 `_timer_id` 置 0，避免框架拒绝重复注册。
- [ ] `handle_timeout()` 中的耗时逻辑放入线程池或再次异步化。

---

## 7. 调试建议

- 使用日志打印 `_timer_type`、状态位以及是否完成异步，便于定位重复触发。
- 在 `handle_timeout()` 开头输出 `t_msg->_timer_id`，判断触发顺序。
- 压测时关注“连接挂起数”，确保超时路径生效。

---

## 8. 关联代码

- `core/base_data_process.*`：定时器注册、异步状态控制。
- `core/protocol_context.*`：Level 2 上下文适配器实现。
- `examples/simple_async_test.cpp`：完整演示异步 + 定时器 + 兜底逻辑。

---

## 9. 相关文档

- `docs/ASYNC_TIMER_QUICK_REF.md`
- `docs/TIMER_USAGE_GUIDE.md`
- `docs/ASYNC_RESPONSE_GUIDE.md`

