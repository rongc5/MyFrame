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
改为**两阶段**，从结构上禁止工作线程接触 Context，且**默认安全**（业务不需要处理错误路径）：
```cpp
// work:       工作线程执行，只算结果，绝不接触 Context
// on_success: work 成功后回投所属网络线程执行，收到结果值 R
template <class R>
void async_response(std::function<R()> work,
                    std::function<void(HttpContext&, R)> on_success);
// void 结果重载：async_response(std::function<void()>, std::function<void(HttpContext&)>)
```
**框架自动兜底**（业务只写成功路径）：
- `work` 抛异常 → **500**，不调用 `on_success`；
- 队列拒绝 → **503**，不调用 `on_success`；
- `on_success` 自身抛异常 → **500**。

实现：
- `work` 投入**框架线程池 `TaskExecutor`**（非每请求建线程；结果/异常存入内部 holder，`work_thunk` 绝不抛出）。
- 完成后构造 `HttpContextTaskMessage`（携带 `apply` + 内部 holder）经 `put_obj_msg(ObjId)` **投回所属网络线程**执行。
- **回投落到网络线程的统一出口 `finalize_once(gen, apply)`**，任一不满足即丢弃（不写任何 Context、不发送）：
  1. 连接 `weak_ptr<base_net_obj>` 仍存活（`put_obj_msg` 连接没了自动丢弃）；
  2. **`request_generation` 未变**（见下）；
  3. **`RequestState != Finalized`**（见 §6.2 状态机）。
- 满足则：置 `Finalized` → 执行 `apply`（holder 有异常则 rethrow，被 `finalize_once` 捕获成 500）→ **恰好触发一次发送**。

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

**实现（提交2，`core/http_router.{h,cpp}`）：**
- 纯路由表：`Router::add/get/post/put/del` 注册，`match(method, raw_path)` 返回
  `MatchResult{ status, handler, params, allow }`，`MatchStatus{Found,NotFound,MethodNotAllowed,BadRequest}`。
- trie 节点含 `static_children`(map) + 唯一 `param_child`；匹配 DFS **静态优先并回溯**
  （静态前缀走进死路时回退到 `{param}`，兼顾优先级与正确性）。
- **先按 `/` 分段、再逐段 URL decode**：`%2F` 解为段内字面 `'/'`、不改变层级；非法百分号编码 → `BadRequest(400)`。
- 冲突检测：重复 `method+path`、同位 `{param}` 名不同（`{id}` vs `{name}`）→ `add` 抛 `std::runtime_error`。
- `Allow` 取终结点 `methods` 的 map 键（**天然去重 + 字典序稳定**）。
- `freeze()` 后 `add` 抛异常；`match` 全程只读、**并发安全无锁**（已并发单测）。
- **Context 绑定推迟**：`route_param()`/把 Router 接入 `on_http_request` 分发放到 energyhub 集成提交，
  本提交保持 Router 与运行时解耦、可独立单测（`test/test_http_router.cpp`）。

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
- 停机（`shutdown`）：**取消尚未开始的排队任务（cancel queued）+ 等待正在执行的任务结束（await in-flight）**；无法强制中断线程，故要求任务本身有界、不长期阻塞。停机后 `submit` 返回 false。
- `work` 因拒绝**未执行**时，也必须让该 HTTP 请求被 finalize（不能永远 pending）；
- **禁止每请求创建线程**。
- 线程数 `MYFRAME_ASYNC_THREADS`（默认 4）、队列上限 `MYFRAME_ASYNC_QUEUE`（默认 1024）可由环境覆盖。

### 6.2 finalize-once 状态机（request 恰好完成一次）
`work()` 抛异常/队列拒绝/连接断开/Keep-Alive 换请求**都不得让请求永远 pending，也不得完成两次**。
内部载体 `MfAsyncHolder<R>` 携带"成功值(`shared_ptr<R>`，不要求 R 可默认构造) | 异常(`exception_ptr`)"，
`work_thunk` 只填 holder、**绝不抛出**；回投的 `apply_thunk` 成功则调 `on_success`、失败则 rethrow。
网络线程统一出口：
```cpp
enum class RequestState { Handling, AsyncPending, Finalized };
void finalize_once(uint64_t gen, const std::function<void(HttpContext&)>& apply) {
    if (gen != request_generation) return;        // 已换请求 -> 丢弃陈旧结果
    if (state == Finalized) return;               // 幂等 -> 只完成一次
    state = Finalized;
    try { if (apply) apply(*this); }              // 成功回调 / rethrow work 异常
    catch (...) { response = 500; }               // 默认安全：work 异常或 on_success 抛 -> 500
    do_notify_complete();                          // 恰好触发一次发送
}
```
- **错误码映射（区分来源）**：
  - **执行器队列拒绝**（work 未执行）-> **503**（在网络线程直接经 `finalize_once` 完成）；
  - **work 自身抛异常** / **on_success 抛异常** -> **500**。
- **`complete_async_response()`** 亦经 `state` 幂等（旧单阶段 API / 流式复用），**重复调用只发送一次**。
- **`MfAsyncHolder<void>`** 单独特化（无值，只携带异常）。

### 6.3 模板接口的类型擦除落点
`HttpContext` 是抽象接口，**模板不能是虚函数**。统一"模板包装器 → 非模板虚函数"：
```cpp
// async：
template <class R> void async_response(work, on_success);  // public 模板包装（头文件，含 void 重载）
virtual void async_response_erased(std::function<void()> work_thunk,
                                   std::function<void(HttpContext&)> apply_thunk) = 0; // Impl 实现
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
- **提交1a** `fix(http): make request context safe`（reset-per-request + typed extensions + generation）
- **提交1b** `fix(http): safe two-phase async_response with TaskExecutor`（执行器 + 两阶段回投骨架）
- **提交1c** `fix(http): enforce async finalize-once and reset full request state`（finalize-once 状态机 + 默认安全 500/503 + `_recv_body`/请求级复位；契约见 §6.2）
- **提交1d** `test(http): cover async disconnect, exception and keep-alive lifecycle`（真实服务集成测试 + finalize_once/executor 单测）
- **提交2** `feat(http): add router and path parameters`
- **提交3** `feat(http): add middleware and JSON helpers`
- **提交4** `refactor(energyhub): migrate customer and ops handlers`

> 每个提交独立、可单独 review/回滚；未提交的 `web_socket_process.cpp` 等无关改动**不混入**（除非确属同一 header bug）。
