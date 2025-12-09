# MyFrame 业务线程访问指南

**文档版本**: 1.0  
**最后更新**: 2025-10-22

---

## 概述

MyFrame 统一协议框架现在支持在 Handler 和 Context 中访问当前业务处理线程 (`base_net_thread`)，这对于需要线程级别控制的业务逻辑很重要。

---

## 访问线程的方法

### Level 1: IApplicationHandler 中访问线程

```cpp
class MyHandler : public IApplicationHandler {
public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        base_net_thread* thread = get_current_thread();

        if (thread) {
            uint32_t thread_index = thread->get_thread_index();
            std::cout << "Current thread: " << thread_index << std::endl;
        }

        res.set_json(R"({"status":"ok"})");
    }
};
```

### Level 2: HttpContext 中访问线程

```cpp
class AdvancedHandler : public IProtocolHandler {
public:
    void on_http_request(HttpContext& ctx) override {
        base_net_thread* thread = ctx.get_thread();

        if (thread) {
            uint32_t thread_index = thread->get_thread_index();
            handle_request_on_thread(ctx, thread_index);
        }

        ctx.response().set_json(R"({"status":"ok"})");
    }

private:
    void handle_request_on_thread(HttpContext& ctx, uint32_t thread_idx) {
        (void)ctx;
        (void)thread_idx;
    }
};
```

### Level 2: WsContext 中访问线程

```cpp
class WsHandler : public IProtocolHandler {
public:
    void on_ws_frame(WsContext& ctx) override {
        base_net_thread* thread = ctx.get_thread();

        if (thread) {
            void* thread_context = get_thread_local_data(thread->get_thread_index());
            process_message(ctx, thread_context);
        }
    }
};
```

---

## 使用场景

### 场景 1: 线程本地存储

某些业务需要在线程级别维护状态，例如数据库连接池：

```cpp
class DatabaseHandler : public IApplicationHandler {
private:
    std::map<uint32_t, DbConnection*> _thread_connections;
    std::mutex _mutex;

public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        base_net_thread* thread = get_current_thread();

        if (!thread) {
            res.status = 500;
            return;
        }

        uint32_t thread_idx = thread->get_thread_index();
        DbConnection* conn = get_thread_connection(thread_idx);

        try {
            auto result = conn->query("SELECT ...");
            res.set_json(result);
        } catch (const std::exception& e) {
            (void)e;
            res.status = 500;
        }
    }

private:
    DbConnection* get_thread_connection(uint32_t thread_idx) {
        std::lock_guard<std::mutex> lock(_mutex);

        auto it = _thread_connections.find(thread_idx);
        if (it == _thread_connections.end()) {
            _thread_connections[thread_idx] = new DbConnection();
        }
        return _thread_connections[thread_idx];
    }
};
```

### 场景 2: 统计信息收集

在线程级别收集性能统计：

```cpp
class StatsHandler : public IApplicationHandler {
private:
    std::map<uint32_t, Stats> _thread_stats;

public:
    void on_http(const HttpRequest& req, HttpResponse& res) override {
        base_net_thread* thread = get_current_thread();

        if (thread) {
            uint32_t idx = thread->get_thread_index();
            _thread_stats[idx].request_count++;
            _thread_stats[idx].total_bytes += req.body.size();
        }

        res.set_json(R"({"status":"ok"})");
    }

    void print_stats() {
        for (const auto& [idx, stats] : _thread_stats) {
            std::cout << "Thread " << idx << ": "
                      << stats.request_count << " requests, "
                      << stats.total_bytes << " bytes" << std::endl;
        }
    }
};
```

### 场景 3: 异步任务分配

基于线程信息进行智能的异步任务分配：

```cpp
class TaskDispatchHandler : public IProtocolHandler {
private:
    std::vector<TaskQueue> _thread_queues;

public:
    void on_http_request(HttpContext& ctx) override {
        base_net_thread* thread = ctx.get_thread();

        if (!thread) {
            ctx.response().status = 500;
            return;
        }

        uint32_t thread_idx = thread->get_thread_index();

        ctx.async_response([this, thread_idx, &ctx]() {
            Task task = parse_request(ctx.request());
            _thread_queues[thread_idx].enqueue(task);

            auto result = execute_task(task);
            ctx.response().set_json(result);
        });
    }
};
```

---

## 线程信息获取

### 从 Handler 获取（Level 1）

```cpp
base_net_thread* thread = handler->get_current_thread();
```

**建议**: 如果 Handler 需要线程信息，考虑升级到 Level 2 使用 Context。

### 从 Context 获取（Level 2）

```cpp
base_net_thread* thread = ctx.get_thread();
```

**保证**: Context 提供的线程指针在请求处理期间有效。

---

## 线程对象的常用方法

```cpp
base_net_thread* thread = ctx.get_thread();

if (thread) {
    uint32_t index = thread->get_thread_index();
    (void)index;
}
```

---

## 最佳实践

### 推荐做法

1. **在 Level 2 中使用**
   ```cpp
   void on_http_request(HttpContext& ctx) override {
       auto thread = ctx.get_thread();
       (void)thread;
   }
   ```

2. **检查指针有效性**
   ```cpp
   if (auto thread = ctx.get_thread()) {
       (void)thread;
   }
   ```

3. **线程安全访问共享资源**
   ```cpp
   std::lock_guard<std::mutex> lock(_mutex);
   _thread_data[thread->get_thread_index()] = value;
   ```

### 避免做法

1. **不要保存线程指针**
   ```cpp
   base_net_thread* _saved_thread = ctx.get_thread();
   ```

2. **不要进行耗时操作而锁持线程**
   ```cpp
   void on_http(const HttpRequest& req, HttpResponse& res) override {
       std::this_thread::sleep_for(std::chrono::seconds(10));
   }
   ```

3. **不要跨线程直接传递线程指针**
   ```cpp
   ctx.async_response([thread = ctx.get_thread()]() {
       auto idx = thread->get_thread_index();
       (void)idx;
   });
   ```

---

## 故障排除

### 问题: `get_thread()` 返回 nullptr

**原因**: 框架中的连接对象没有实现线程访问接口。

**解决方案**:
1. 检查 `base_net_obj` 是否有 `get_thread()` 方法
2. 查看框架文档了解线程信息的获取方式
3. 作为备选，使用线程本地存储 (TLS)

### 问题: 线程索引超出范围

**原因**: Worker 线程数量与查询的索引不匹配。

**解决方案**:
```cpp
if (thread) {
    uint32_t idx = thread->get_thread_index();
    if (idx < _thread_data.size()) {
    }
}
```

---

## 集成示例

### 完整的统计系统

```cpp
class StatisticsHandler : public IProtocolHandler {
private:
    struct ThreadStats {
        std::atomic<int64_t> total_requests{0};
        std::atomic<int64_t> total_bytes_sent{0};
        std::atomic<int64_t> total_bytes_received{0};
        std::vector<std::chrono::milliseconds> response_times;
    };

    std::vector<ThreadStats> _stats;
    std::mutex _stats_mutex;

public:
    StatisticsHandler(int thread_count) : _stats(thread_count) {}

    void on_http_request(HttpContext& ctx) override {
        auto start = std::chrono::high_resolution_clock::now();

        if (auto thread = ctx.get_thread()) {
            uint32_t idx = thread->get_thread_index();

            _stats[idx].total_requests++;
            _stats[idx].total_bytes_received += ctx.request().body.size();

            ctx.response().set_json(R"({"status":"ok"})");

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<
                std::chrono::milliseconds>(end - start);

            _stats[idx].total_bytes_sent += 
                ctx.response().body.size();

            {
                std::lock_guard<std::mutex> lock(_stats_mutex);
                _stats[idx].response_times.push_back(duration);
            }
        }
    }

    void print_report() {
        std::cout << "=== Thread Statistics ===" << std::endl;
        for (size_t i = 0; i < _stats.size(); ++i) {
            const auto& stats = _stats[i];
            std::cout << "Thread " << i << ":\n"
                      << "  Requests: " << stats.total_requests << "\n"
                      << "  Bytes received: " << stats.total_bytes_received << "\n"
                      << "  Bytes sent: " << stats.total_bytes_sent << "\n";
        }
    }
};
```

---

## 参考资源

- `core/base_net_thread.h` - 线程对象接口
- `core/app_handler_v2.h` - Level 1 Handler 接口
- `core/protocol_context.h` - Level 2 Context 接口

---

**文档完成时间**: 2025-10-22
