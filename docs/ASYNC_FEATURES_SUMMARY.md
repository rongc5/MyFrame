# MyFrame 异步能力实现摘要

**文档版本**: 1.0  
**最后更新时间**: 2025-10-22

---

## 1. 本轮交付内容

- 完善 Level 2 (`HttpContext`) 异步接口：`async_response()`、`complete_async_response()`。
- 对 Level 3 (`http_base_data_process`) 异步流程进行文档化，明确 `set_async_response_pending()` 的使用时机。
- 梳理定时器（`timer_msg` + `add_timer()` + `handle_timeout()`）与异步流程的协同方式。
- 更新示例与文档，覆盖典型场景（数据库查询、消息队列、双定时器保护、心跳）。

---

## 2. 关键 API 速览

| 模块 | 接口 | 说明 |
| ---- | ---- | ---- |
| `HttpContext` | `async_response(std::function<void()>)` | 在后台线程运行耗时任务。 |
| `HttpContext` | `complete_async_response()` | 完成异步流程并触发发送。 |
| `base_data_process` | `add_timer(std::shared_ptr<timer_msg>)` | 注册定时器。 |
| `base_data_process` | `handle_timeout(std::shared_ptr<timer_msg>&)` | 定时器回调入口。 |
| `http_base_data_process` | `set_async_response_pending(bool)` | 控制响应是否延迟发送。 |

---

## 3. 行为约束

- `complete_async_response()` **必须**在所有成功或失败路径调用，避免连接悬挂。
- 注册定时器前，`timer_msg::_obj_id` 需与当前连接匹配，否则框架会拒绝。
- 复制 `timer_msg` 时请清除 `_timer_id`，让框架重新分配。
- `handle_timeout()` 内避免长时间阻塞；如需执行耗时操作，重新投递到线程池或再次注册定时器。

---

## 4. 兼容性

- 旧版 `base_data_process` 继承模型保持不变，已有实现无需修改。
- Level 1 (`IApplicationHandler`) 默认仍为同步。希望使用异步/定时器的业务需要升级到 Level 2 或 Level 3。
- 与现有 `MultiProtocolFactory` 兼容，可逐步将旧业务迁移到新的异步接口。

---

## 5. 推荐测试

1. **单元测试**：覆盖成功、失败、超时、重复触发场景。
2. **集成测试**：使用示例项目验证 HTTP 异步路径是否按预期返回。
3. **压力测试**：重点观察异步队列堆积及定时器触发顺序。
4. **资源监控**：关注长连接数、线程数、CPU 使用率，确认异步逻辑未造成泄漏。

---

## 6. 参考资料

- `docs/ASYNC_TIMER_QUICK_REF.md`
- `docs/TIMER_USAGE_GUIDE.md`
- `docs/ASYNC_RESPONSE_GUIDE.md`
- `docs/ASYNC_AND_TIMER_COMPLETE.md`
- `examples/simple_async_test.cpp`

