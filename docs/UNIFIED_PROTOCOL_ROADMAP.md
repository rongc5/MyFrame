# MyFrame 统一协议架构 - 实施路线图

## 目标

废弃 `MultiProtocolFactory + IAppHandler` 的简化设计，采用三层架构（Level 1/2/3）+ Context 思想的统一协议处理框架。

---

## Phase 1: 基础设施搭建（预计 2 周）

### 1.1 核心接口定义

#### 文件清单
```
core/app_handler.h                    // Level 1 接口
core/protocol_context.h               // Level 2 接口
core/unified_protocol_factory.h       // 统一工厂
core/protocol_detector.h              // 协议检测器
```

#### 任务清单
- [ ] 定义 `IApplicationHandler` 接口
  - `on_http(HttpRequest, HttpResponse)`
  - `on_ws(WsFrame, WsFrame)`
  - `on_binary(BinaryRequest, BinaryResponse)`
  - 生命周期钩子

- [ ] 定义 Request/Response 数据结构
  - `HttpRequest` / `HttpResponse`
  - `WsFrame`
  - `BinaryRequest` / `BinaryResponse`
  - `ConnectionInfo`

- [ ] 定义 `UnifiedProtocolFactory` 类
  - 协议注册接口（三个层次）
  - IFactory 接口实现
  - 内部协议表管理

- [ ] 实现 `ProtocolDetector` 类
  - 缓冲区管理
  - 协议遍历检测
  - 代理创建和转发
  - 超时和安全限制

### 1.2 Level 1 适配器实现

#### 文件清单
```
core/protocol_adapters/http_application_adapter.h
core/protocol_adapters/http_application_adapter.cpp
core/protocol_adapters/ws_application_adapter.h
core/protocol_adapters/ws_application_adapter.cpp
core/protocol_adapters/binary_application_adapter.h
core/protocol_adapters/binary_application_adapter.cpp
```

#### 任务清单
- [ ] HTTP Level 1 适配器
  - `HttpApplicationAdapter` (继承 http_res_process)
  - `HttpApplicationDataProcess` (继承 http_base_data_process)
  - Request/Response 转换
  - 调用 IApplicationHandler::on_http()

- [ ] WebSocket Level 1 适配器
  - `WsApplicationAdapter` (继承 web_socket_res_process)
  - `WsApplicationDataProcess` (继承 web_socket_data_process)
  - Frame 转换
  - 调用 IApplicationHandler::on_ws()

- [ ] Binary Level 1 适配器
  - `BinaryApplicationAdapter` (继承 base_data_process)
  - 简单 TLV 格式解析
  - 调用 IApplicationHandler::on_binary()

### 1.3 单元测试

#### 文件清单
```
test/test_unified_factory.cpp
test/test_protocol_detector.cpp
test/test_http_adapter.cpp
test/test_ws_adapter.cpp
test/test_binary_adapter.cpp
```

#### 任务清单
- [ ] UnifiedProtocolFactory 测试
  - 协议注册
  - 优先级排序
  - 重复注册检测

- [ ] ProtocolDetector 测试
  - HTTP 检测
  - WebSocket 检测
  - 二进制协议检测
  - 缓冲区溢出保护

- [ ] 适配器测试
  - HTTP 请求/响应转换
  - WebSocket 帧转换
  - 二进制数据转换

### 1.4 示例程序

#### 文件清单
```
examples/unified_http_server.cpp      // 简单 HTTP 服务器
examples/unified_ws_server.cpp        // 简单 WebSocket 服务器
examples/unified_mixed_server.cpp     // HTTP + WS 混合
```

---

## Phase 2: Level 2 支持（预计 2 周）

### 2.1 Context 类实现

#### 文件清单
```
core/http_context.h
core/http_context.cpp
core/ws_context.h
core/ws_context.cpp
core/binary_context.h
core/binary_context.cpp
```

#### 任务清单
- [ ] HttpContext 实现
  - 请求/响应访问
  - 异步响应支持
  - 流式响应支持
  - 协议升级（upgrade_to_websocket）
  - 用户数据管理
  - 连接控制

- [ ] WsContext 实现
  - 帧访问
  - 发送控制（text/binary/ping/pong/close）
  - 状态查询
  - 广播支持（需配合 Hub）
  - 用户数据管理

- [ ] BinaryContext 实现
  - 状态机支持
  - 消息访问
  - 异步发送
  - 流式处理
  - 用户数据管理

### 2.2 Level 2 适配器实现

#### 文件清单
```
core/protocol_adapters/http_context_adapter.h
core/protocol_adapters/http_context_adapter.cpp
core/protocol_adapters/ws_context_adapter.h
core/protocol_adapters/ws_context_adapter.cpp
core/protocol_adapters/binary_context_adapter.h
core/protocol_adapters/binary_context_adapter.cpp
```

#### 任务清单
- [ ] HTTP Context 适配器
  - Context 对象构造
  - 调用 IProtocolHandler::on_http_request()
  - 异步响应机制
  - 流式响应机制

- [ ] WebSocket Context 适配器
  - Context 对象构造
  - 调用 IProtocolHandler::on_ws_frame()
  - 状态管理

- [ ] Binary Context 适配器
  - Context 对象构造
  - 状态机集成
  - 调用 IProtocolHandler::on_binary_message()

### 2.3 异步响应机制

#### 文件清单
```
core/async_response_manager.h
core/async_response_manager.cpp
```

#### 任务清单
- [ ] 异步任务队列
- [ ] 线程池集成
- [ ] 超时控制
- [ ] 错误处理

### 2.4 集成测试

#### 文件清单
```
test/test_http_context.cpp
test/test_ws_context.cpp
test/test_async_response.cpp
```

#### 任务清单
- [ ] HttpContext 功能测试
- [ ] WsContext 功能测试
- [ ] 异步响应测试
- [ ] 流式响应测试

### 2.5 示例程序

#### 文件清单
```
examples/async_http_server.cpp        // 异步 HTTP
examples/ws_auth_server.cpp           // 带认证的 WebSocket
examples/binary_rpc_server.cpp        // 二进制 RPC
```

---

## Phase 3: 文档和优化（预计 1 周）

### 3.1 API 文档

#### 文件清单
```
docs/API_REFERENCE.md                 // API 参考
docs/LEVEL1_GUIDE.md                  // Level 1 使用指南
docs/LEVEL2_GUIDE.md                  // Level 2 使用指南
docs/LEVEL3_GUIDE.md                  // Level 3 使用指南
docs/MIGRATION_GUIDE.md               // 迁移指南
```

#### 任务清单
- [ ] 完整 API 文档
- [ ] 分层使用指南
- [ ] 最佳实践
- [ ] 常见问题解答
- [ ] 性能优化建议

### 3.2 示例和教程

#### 文件清单
```
examples/tutorial_01_simple_http.cpp
examples/tutorial_02_simple_ws.cpp
examples/tutorial_03_mixed_protocol.cpp
examples/tutorial_04_async_http.cpp
examples/tutorial_05_ws_broadcast.cpp
examples/tutorial_06_binary_protocol.cpp
examples/tutorial_07_custom_protocol.cpp
```

### 3.3 性能测试

#### 文件清单
```
benchmark/bench_unified_factory.cpp
benchmark/bench_protocol_detection.cpp
benchmark/bench_http_adapter.cpp
benchmark/bench_ws_adapter.cpp
```

#### 任务清单
- [ ] 协议检测性能测试
- [ ] 适配器性能测试
- [ ] 内存使用测试
- [ ] 与旧框架对比

---

## Phase 4: 向后兼容和发布（预计 1 周）

### 4.1 废弃旧接口

#### 任务清单
- [ ] 标记 `MultiProtocolFactory` 为 deprecated
- [ ] 标记旧的 `IAppHandler` 为 deprecated
- [ ] 添加编译警告
- [ ] 提供迁移脚本

### 4.2 兼容性测试

#### 任务清单
- [ ] 确保 Level 3（base_data_process）完全兼容
- [ ] 测试现有项目（如 stock_driver）
- [ ] 回归测试

### 4.3 发布准备

#### 任务清单
- [ ] 版本号更新（v2.0.0）
- [ ] CHANGELOG 编写
- [ ] Release Notes
- [ ] 发布 Beta 版

---

## 关键里程碑

| 里程碑 | 预计完成时间 | 交付物 |
|--------|-------------|--------|
| M1: 核心框架完成 | Week 2 | UnifiedProtocolFactory + Level 1 适配器 + 单元测试 |
| M2: Level 2 支持 | Week 4 | Context 类 + Level 2 适配器 + 异步支持 |
| M3: 文档完成 | Week 5 | 完整文档 + 教程 + 性能报告 |
| M4: 发布准备 | Week 6 | Beta 版本 + 迁移指南 |
| M5: 正式发布 | Week 8 | v2.0.0 Release |

---

## 风险和应对

### 风险1: 性能下降
**应对**:
- 提前进行性能基准测试
- 优化协议检测逻辑（缓存、快速路径）
- 避免不必要的内存拷贝

### 风险2: 向后兼容问题
**应对**:
- 保留 Level 3 完全兼容旧框架
- 提供自动化迁移工具
- 详细的迁移文档

### 风险3: API 复杂度
**应对**:
- 提供丰富的示例
- 分层文档（入门/进阶/高级）
- 视频教程

### 风险4: 测试覆盖不足
**应对**:
- 单元测试覆盖率 > 80%
- 集成测试覆盖所有场景
- 压力测试和稳定性测试

---

## 开发约定

### 代码规范
- C++11 标准
- 遵循现有代码风格
- 所有公开 API 必须有文档注释
- 单元测试必须通过

### 提交规范
```
feat: 添加 HttpContext 类
fix: 修复协议检测器缓冲区溢出
docs: 更新 API 文档
test: 添加 WebSocket 适配器测试
refactor: 重构协议注册机制
```

### 分支管理
```
main                    // 稳定分支
develop                 // 开发分支
feature/unified-proto   // 功能分支（本次开发）
```

---

## 资源需求

### 开发人员
- 核心开发: 1-2 人
- 测试: 1 人
- 文档: 1 人

### 时间估算
- 总工作量: 6-8 周
- 并行开发可压缩至 4-6 周

### 依赖
- 现有 myframe 框架
- C++11 编译器
- 测试框架（可选：Google Test）
- 文档工具（Markdown）

---

## 验收标准

### 功能完整性
- [ ] Level 1 支持 HTTP/WebSocket/Binary
- [ ] Level 2 支持 Context 和异步
- [ ] Level 3 完全兼容旧框架
- [ ] 统一协议注册机制
- [ ] 协议检测正确性 100%

### 性能要求
- [ ] 协议检测延迟 < 1ms
- [ ] 适配器开销 < 5%
- [ ] 内存增长 < 10%

### 质量要求
- [ ] 单元测试覆盖率 > 80%
- [ ] 无内存泄漏
- [ ] 无线程安全问题
- [ ] 文档完整准确

### 用户体验
- [ ] 简单场景 < 10 行代码
- [ ] 清晰的错误信息
- [ ] 丰富的示例
- [ ] 平滑的迁移路径

---

## 后续规划

### v2.1 (Q1 2026)
- 中间件支持
- 插件系统
- 更多协议支持（MQTT, Redis等）

### v2.2 (Q2 2026)
- 协议热插拔
- 动态协议配置
- 监控和诊断工具

### v3.0 (Q3 2026)
- 全面重构为 C++17/20
- 协程支持
- 零拷贝优化

---

**文档版本**: 1.0
**创建时间**: 2025-10-22
**负责人**: MyFrame Team
**状态**: 待启动
