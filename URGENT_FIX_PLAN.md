# 紧急修复计划

**目标**: 让至少一个示例能够编译和运行

---

## 当前状态

? **所有 unified 示例都无法编译**

原因：
1. Level 2 适配器 (`http_context_adapter`, `ws_context_adapter`) 有严重bug
2. Level 1 适配器依赖有问题的工厂方法
3. 接口不匹配问题遍布代码

---

## 紧急方案：回退到可工作状态

### 步骤 1: 暂时禁用 Level 2 适配器

在 `CMakeLists.txt` 中注释掉有问题的文件：

```cmake
# 暂时禁用 Level 2 适配器
# core/protocol_adapters/http_context_adapter.cpp
# core/protocol_adapters/ws_context_adapter.cpp
```

### 步骤 2: 只编译 Level 1 示例

```bash
cd build
make http_server  # 传统示例（已知可工作）
make async_http_demo  # 异步示例（已知可工作）
```

---

## 中期方案：修复 Level 1 适配器

只保留最基本的功能：
1. HTTP Application Adapter（已基本可用）
2. WebSocket Application Adapter
3. Binary Application Adapter（需要小修复）

---

## 长期方案：重新实现 Level 2

需要重新设计，考虑：
1. 与现有框架的真实接口匹配
2. 完整的请求/响应生命周期
3. 真正的异步支持
4. 完整的测试覆盖

---

## 建议给用户

**暂时使用传统方式**：

```cpp
// 使用已知可工作的方式
class MyHttpProcess : public http_base_data_process {
    // 直接继承，完全控制
};
```

**或者使用 Level 1**（如果修复后）：

```cpp
// 简单的协议无关处理
class MyHandler : public IApplicationHandler {
    void on_http(const HttpRequest& req, HttpResponse& res) {
        // 简单处理
    }
};
```

---

**创建时间**: 2025-10-22

