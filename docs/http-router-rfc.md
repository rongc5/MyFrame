# myframe HTTP Router / Middleware / Context RFC（短设计）

> 基于**现有** `HttpContext`（core/protocol_context.h）扩展，不新建 Context。
> 复用已有 `HttpContextTaskMessage`（op 0x48435431）+ `send_msg` 做跨线程回投。

## 1. Context 生命周期（提交1）

- **语义**：`HttpContext` 是"当前请求"的视图。连接（Keep-Alive）复用 Impl 时，框架在**每个新请求开始**调用内部 `reset_for_request()`：
  - 清空 `response`、`route_params`、`extensions`；重置流式/异步标志。
- 业务永远看不到上一个请求的残留（杜绝跨请求租户泄漏）。

### 类型安全扩展（替代裸 void*）
新增到 `HttpContext`：
```cpp
// 拥有对象、按类型取；每请求 reset 时析构清空
template <class T, class... A> T& set_ext(const std::string& key, A&&... args);
template <class T> T* get_ext(const std::string& key) const;   // 不存在/类型不符返回 nullptr
```
- 底层：`unordered_map<string, {type_index, shared_ptr<void>}>`；`get_ext<T>` 校验 type_index。
- **类型归一**：内部统一（**保持公开头 C++11 兼容**，不用 `remove_cv_t`）：
  ```cpp
  typedef typename std::remove_cv<typename std::remove_reference<T>::type>::type StoredType;
  ```
  避免 `Principal` / `const Principal` / `Principal&` 被当成不同类型。
- `set_user_data/get_user_data(void*)` 保留但标注弃用；新 Router/Middleware/示例禁止再用，下个破坏性版本删除。
  - **C++11 兼容**：`[[deprecated]]` 是 C++14，公开头用兼容宏：
    ```cpp
    #if __cplusplus >= 201402L
    #  define MYFRAME_DEPRECATED(msg) [[deprecated(msg)]]
    #elif defined(__GNUC__) || defined(__clang__)
    #  define MYFRAME_DEPRECATED(msg) __attribute__((deprecated(msg)))
    #else
    #  define MYFRAME_DEPRECATED(msg)
    #endif
    ```

## 2. 异步生命周期（提交1）—— 两阶段 API

**只"不捕获裸 this"不够**：业务仍可能 `[&ctx]` 捕获，导致工作线程跨线程改 response / 悬空。
改为**两阶段**，从结构上禁止工作线程接触 Context：
```cpp
// work：工作线程执行，只算结果，绝不接触 Context
// done：回投到所属网络线程后执行，此时才能访问 Context
template <class R>
void async_response(std::function<R()> work,
                    std::function<void(HttpContext&, R)> done);
```
实现：
- `work` 投入**框架线程池**（非每请求建线程）。
- 完成后构造 `HttpContextTaskMessage`（携带 `done` + 结果）经 `send_msg` **投递回网络线程**执行。
- **回投时三重校验**，任一不满足即丢弃结果（不写任何 Context）：
  1. 连接 `weak_ptr<base_net_obj>` 仍存活；
  2. **`request_generation` 未变**（见下）；
  3. 该请求尚未 finalized（响应未发出）。

### request generation（关键）
连接可能存活但 Keep-Alive 已开始处理**下一条请求**，旧异步结果不能写进新请求的 Context。
- `reset_for_request()` 时 `++request_generation`（`uint64_t`）。
- 异步任务发起时记录当时的 generation；回投时比对。不等 → 丢弃。

## 3. Router（提交2）

```cpp
myframe::Router router;
router.get ("/api/v1/devices/{id}", handler);
router.post("/api/v1/devices/{id}/commands", handler);
router.use(globalMw);                          // 全局中间件
router.get("/x", h).use(authMw).use(roleMw);   // 每路由中间件
```
handler 签名：`std::function<void(HttpContext&)>`。

**匹配规则（v1 冻结）：**
- 段式匹配；**静态段优先于 `{param}`**。
- `{id}` 捕获 → `ctx.route_param("id")`（新增到 Context）；参数值做 **URL decode**。
- **query string 不参与匹配**（先剥掉 `?...`）。
- 注册期发现**重复/冲突路由 → 启动报错**（返回错误码/抛异常，不静默覆盖）。
- 不匹配 → **404**；路径匹配但方法不符 → **405 + `Allow` 头**。
- **尾部 `/`**：规范化为无尾斜杠（`/x/` 等价 `/x`）。
- **通配 `*`**：v1 不支持。
- 注册完成后**冻结**：构建只读结构（静态用 hash、含参用 trie），运行期只读**无锁**。

## 4. Middleware（提交3）

```cpp
using BeforeMiddleware = std::function<bool(HttpContext&)>;  // false = 短路(响应已设好)
using AfterMiddleware  = std::function<void(HttpContext&)>;
router.use(requestId); router.use(cors); router.use(accessLog);
route.use(authenticate).use(requireRole("customer"));
```
- 执行链：全局 before → 路由 before → handler；任一 before 返回 false 即短路。
- **after 语义（严格）**：
  - **逆序**执行；
  - 鉴权短路、handler 抛异常、handler 正常返回**三种情况都执行**；
  - **异步请求**：after 必须在**真正完成响应时**执行（`complete_async_response` 后），**不能**在 handler 启动异步任务后立即执行；
  - 每个 after **最多执行一次**。
- v1 **不引入 `next()` 续延式**，避免异步生命周期复杂化。
- **统一错误处理**：handler/middleware 抛异常 → 框架 catch → 500 标准响应（之后仍跑 after）。
- 框架**只**提供 chain / short-circuit / extensions / pre-post / 错误处理。
- **业务层**（energyhub）实现 `authenticate`：解析 JWT → `ctx.set_ext<Principal>("principal", {...})`；`requireRole(r)` 读 `get_ext<Principal>`。
```cpp
struct Principal { int64_t tenant_id; int64_t user_id; std::string role; };  // 属 energyhub，不进 myframe
```

## 5. JSON 便捷（提交3）

```cpp
auto j = ctx.request().parse_json();     // 返回可判失败的包装(如 optional<Document> 或 JsonResult)
if (!j) return ctx.json_error(400, "invalid_json");
ctx.response().set_json(*j);             // set_json 增加 Document 重载；自动设 Content-Type
```
- `parse_json` 校验：解析错误、UTF-8；失败**不静默返回空对象**。
- **Content-Type 接受**：`application/json`、`application/json; charset=utf-8`、`application/*+json`。
- `ctx.json_error(code,msg)`：标准错误体 `{"error":msg}` + 正确 Content-Type。

### 请求体大小限制必须在接收阶段（不是 parse_json）
否则超大 body 已完整进 `_recv_body`，挡不住内存攻击。提前拒绝的位置：
- 解析 `Content-Length` 时；
- chunked/body 累计接收时；
- 超限**立即 413 Payload Too Large**。
- `parse_json()` 再做一次防御性检查即可（二次兜底）。

## 已定（决策）

1. **尾斜杠**：规范化——`/x` 与 `/x/` 视为同一路由，**不重定向**；根路径 `/` 特殊保留。
2. **Middleware**：`bool(HttpContext&)` before + `void(HttpContext&)` after（语义见 §4）；不引入 `next()`。
3. **Extensions**：`type_index + shared_ptr<void>`，`StoredType` 归一。
4. **`set_user_data(void*)`**：保留 + `[[deprecated]]`，下个破坏性版本删。
5. **Header 修复单独提交（提交0）**，不并入 Context 提交；便于 review/回滚/bisect。

## 6. 提交1 接口契约（实现前必须定清）

### 6.1 执行器契约（TaskExecutor）
仓库无现成通用阻塞任务池：`base_net_thread::_worker_pool` 是**网络线程集合**，不是业务任务池。
新增 **`TaskExecutor`** 抽象（由 HTTP factory/context 共享，**非裸全局单例**）：
- 进程级**固定线程数**；
- **有界任务队列** + 队列满**拒绝策略**（拒绝即视为异步失败 → **503 Service Unavailable**，见 6.2；不得每请求建线程）；
- 停机：停止接收新任务，等待或取消在飞任务；
- `work` 因拒绝**未执行**时，也必须让该 HTTP 请求被 finalize（不能永远 pending）；
- **禁止每请求创建线程**。

### 6.2 work 异常语义（request 只 finalize 一次）
`work()` 抛异常/队列拒绝**不得让请求永远 pending**。两阶段接口把"结果或异常"都投回网络线程：
```cpp
template <class R>
void async_response(std::function<R()> work,
                    std::function<void(HttpContext&, AsyncResult<R>)> done);
// AsyncResult<R>: 成功(值) | 异常(exception_ptr) | 被拒绝
```
- 框架统一捕获 `std::exception_ptr`；`done` 在网络线程按结果生成响应。
- **错误码映射（区分来源）**：
  - **执行器队列拒绝**（work 未执行）→ **503 Service Unavailable**；
  - **work 自身抛异常**（业务错误）→ **500 Internal Server Error**（done 可覆盖）。
- **保证**：成功 / 异常 / 队列拒绝，request 都**只 finalize 一次**。
- **`AsyncResult<void>`**：`R=void` 无值，需**单独特化**（只携带成功/异常/拒绝，不带值）
  或在接口层**明确禁止 `R=void`**（`static_assert`）。二选一，v1 建议提供 void 特化。

### 6.3 模板接口的类型擦除落点
`HttpContext` 是抽象接口，**模板不能是虚函数**。统一"模板包装器 → 非模板虚函数"：
```cpp
// async：
template <class R> void async_response(work, done);      // public 模板包装（头文件）
virtual void async_response_erased(std::function<void()> work_thunk,
                                   std::function<void(HttpContext&)> done_thunk) = 0; // Impl 实现
// extensions：
template <class T,class...A> T& set_ext(key,A&&...);      // 包装
template <class T> T* get_ext(key) const;                // 包装
virtual void  set_ext_erased(key, std::type_index, std::shared_ptr<void>) = 0;
virtual std::shared_ptr<void> get_ext_erased(key, std::type_index) const = 0;
```
模板包装器负责类型归一 + 打包/校验，虚函数只存取已擦除的 entry。

### 6.4 C++ 标准
仓库现状冲突：CMakeLists=C++17、docs/AGENTS.md=C++11、test/run_integration_test.sh=C++11。
- **本轮公开头文件尽量 C++11 兼容**（如 6.1-6.3 与 StoredType 写法）；
- C++ 标准统一**单独处理**，不混进 Context 安全提交。

## 提交次序（最终）

- **提交0** `fix(http): normalize header OWS and support case-insensitive lookup`
- **提交0.1** `build(cmake): make examples reproducible from a clean checkout` + `test(http): header OWS/case regression`
- **提交1** `fix(http): make request context and async lifetime safe`（reset + typed ext + 两阶段 async + generation；接口契约见 §6）
- **提交2** `feat(http): add router and path parameters`
- **提交3** `feat(http): add middleware and JSON helpers`
- **提交4** `refactor(energyhub): migrate customer and ops handlers`

> 每个提交独立、可单独 review/回滚；未提交的 `web_socket_process.cpp` 等无关改动**不混入**（除非确属同一 header bug）。
