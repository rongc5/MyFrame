# 异步 HTTP 响应示例

## 概述

`async_http_server_demo.cpp` 演示了如何使用 myframe 框架的异步响应接口来处理需要延迟响应的 HTTP 请求（如数据库查询、远程 API 调用等）。

## 核心变化

框架层已经具备以下异步支撑（从 rxtracenetcap 移植）：

### 1. `http_base_data_process` (core/http_base_data_process.h/cpp)
- 新增 `async_response_pending()` 查询异步状态
- 新增 `set_async_response_pending(bool)` 设置异步状态（protected）
- 内部用 `_async_response_pending` 保存状态，默认 `false`

### 2. `http_base_process` (core/http_base_process.h/cpp)
- 保留 `change_http_status()` 修改 HTTP 状态
- 新增 `notify_send_ready()` 通知框架发送响应

### 3. `http_res_process` (core/http_res_process.cpp)
- `recv_finish()` 在调用 `msg_recv_finish()` 后：
  - 若 `async_response_pending()` 为 `true`，直接返回等待异步结果
  - 否则调用 `notify_send_ready()` 立即发送响应

## 使用方法

### 基本流程

1. **接收请求时**：在 `msg_recv_finish()` 中判断是否需要异步处理
   ```cpp
   virtual void msg_recv_finish() override {
       if (需要异步处理) {
           // 1. 设置异步标志
           set_async_response_pending(true);

           // 2. 启动异步操作（定时器/线程/异步IO等）
           auto timer = std::make_shared<timer_msg>();
           timer->_timer_type = ASYNC_QUERY_TIMER;
           timer->_time_length = 1000;  // 1秒

           // 3. 必须设置 _obj_id
           if (auto conn = get_base_net()) {
               timer->_obj_id = conn->get_id()._id;
           }

           add_timer(timer);
           // 不调用 notify_send_ready()，等待异步完成
       } else {
           // 同步处理：立即生成响应
           generate_response();
           // 框架会自动调用 notify_send_ready()
       }
   }
   ```

2. **异步完成时**：在回调中完成响应并通知框架
   ```cpp
   virtual void handle_timeout(std::shared_ptr<timer_msg>& t_msg) override {
       if (t_msg->_timer_type == ASYNC_QUERY_TIMER) {
           // 1. 生成响应
           generate_response();

           // 2. 使用便利方法完成异步响应（清除标志 + 通知发送）
           complete_async_response();

           // 或者分步调用：
           // set_async_response_pending(false);
           // _base_process->notify_send_ready();
       }
   }
   ```

## 编译与运行

### 编译
```bash
cd /home/rong/myframe/build
cmake ..
make async_http_demo -j4
```

### 运行
```bash
cd /home/rong/myframe/build/examples
./async_http_demo 8090
```

### 测试

同步请求（立即响应）：
```bash
curl http://127.0.0.1:8090/sync
# 耗时 < 10ms
```

异步请求（1秒延迟）：
```bash
time curl http://127.0.0.1:8090/async
# 耗时 ~1s
```

查看时间差异：
```bash
time curl http://127.0.0.1:8090/sync    # real 0m0.004s
time curl http://127.0.0.1:8090/async   # real 0m1.015s
```

## 关键注意事项

1. **线程模型**
   - ✅ `_async_response_pending` 仅在连接所属的网络线程内读写
   - ✅ 定时器回调通过事件循环回到同一线程执行
   - ✅ 框架采用单线程事件循环模型，状态访问顺序执行
   - ℹ️ 如需在独立线程池处理业务，应通过消息/定时器投递回网络线程

2. **便利方法**
   - ✅ 已提供 `complete_async_response()` 简化调用
   - 替代手动调用 `set_async_response_pending(false)` + `notify_send_ready()`
   ```cpp
   // 推荐：一步完成
   complete_async_response();

   // 或分步调用（等价）
   set_async_response_pending(false);
   _base_process->notify_send_ready();
   ```

3. **定时器使用要点**
   - 必须设置 `timer->_obj_id`，否则 `add_timer()` 会失败
   ```cpp
   timer->_obj_id = get_base_net()->get_id()._id;
   ```

### ✅ 最佳实践

1. **保存请求上下文**：异步处理时，在成员变量中保存请求信息
   ```cpp
   std::string _async_url;  // 保存 URL
   std::string _async_body; // 保存请求体
   ```

2. **错误处理**：异步操作失败时，也要返回错误响应
   ```cpp
   if (异步操作失败) {
       generate_error_response();
       complete_async_response();  // 或分步调用
   }
   ```

3. **超时保护**：设置超时定时器，避免请求永久挂起
   ```cpp
   auto timeout_timer = std::make_shared<timer_msg>();
   timeout_timer->_timer_type = ASYNC_TIMEOUT_TIMER;
   timeout_timer->_time_length = 5000;  // 5秒超时
   ```

## 示例代码结构

```
async_http_server_demo.cpp
├── async_http_data_process      # 数据处理类
│   ├── msg_recv_finish()        # 接收完成，启动异步
│   ├── handle_timeout()         # 定时器触发，完成响应
│   └── generate_response()      # 生成 HTTP 响应
│
└── async_http_factory           # 工厂类
    ├── net_thread_init()        # 线程初始化
    ├── handle_thread_msg()      # 创建连接和数据处理器
    └── on_accept()              # 接收新连接
```

## 服务器日志示例

同步请求：
```
[Request] GET /sync
[Sync] Processing sync request for /sync
```

异步请求：
```
[Request] GET /async
[Async] Starting async operation for /async
[Async] Async flag set, timer started (1s delay)
[Async] Timer triggered, completing async operation
[Async] Response ready, complete_async_response() called
```

## 适用场景

✅ 适合异步处理的场景：
- 数据库查询（连接池异步查询）
- 远程 API 调用（HTTP/RPC 客户端）
- 缓存查询（Redis/Memcached）
- 文件 I/O（异步读写）
- 消息队列处理（发送/接收消息）

❌ 不适合异步处理的场景：
- 简单的内存计算
- 快速的数据转换
- 配置读取等同步操作

## 总结

这个异步接口设计简洁实用：
1. ✅ **同步兼容**：不影响现有同步代码
2. ✅ **易于使用**：设置标志 → 启动异步 → 完成响应，简单明了
3. ✅ **便利方法**：`complete_async_response()` 一步完成清除标志和通知发送
4. ✅ **灵活扩展**：可用于定时器、线程、异步 I/O 等多种场景
5. ✅ **线程安全**：基于单线程事件循环模型，状态访问顺序执行，无需额外同步
6. ⚠️ **需注意**：定时器 `_obj_id` 设置、错误处理、超时保护

后续可根据业务需要，在新的框架业务模块中调用这些接口。
