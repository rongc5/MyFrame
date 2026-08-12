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
- **路由级 middleware**：`router.get(path, h).use(mw)` / `.use_before()` / `.use_after()`——`add` 返回
  `Router::Route` 代理，路由的 `Middleware` 列表存于 trie 的 `Entry`；`match` 的 `MatchResult.route_mws`
  指向它（freeze 后只读稳定）。分发时把**全局 + 路由级**组成**同一条 `MiddlewareChain`**：短路/异常/
  异步/after-once 统一语义（不再有绕过契约的业务包装）。
- **错误渲染可配置**：`RoutingOptions.render_error(ctx, status, code)`——框架默认 `{"error":"<code>"}`
  （`not_found`/`method_not_allowed`/`bad_request` 中性错误码）；业务可覆盖文案（如 energyhub 把
  `not_found` 渲染成旧的 `"not found"`），**不改框架默认、不影响其他用户**。

**语义边界（提交2 评审拍板）：**
- **方法字面匹配（纯 Router）**：`Router::match` 只按注册的字面方法匹配；HEAD/OPTIONS **不在 Router 内合成**，
  `Allow` 只含字面方法。合成由**分发层**（Commit 4，`RoutingHandler`）实现，策略如下（已拍板）：
  - **显式注册的 `HEAD`/`OPTIONS` 路由优先**（先按字面 match）；
  - 无 `HEAD` 但有 `GET` → **复用 GET handler**，最终发送阶段**抑制 body**、保留本应返回的**状态与 headers**（含按 GET body 计算的 `Content-Length`）；
  - 无显式 `OPTIONS` → 自动 **204 + `Allow`**；
  - `Allow` 组装：存在 `GET` 则同时列 `GET, HEAD`；自动 `OPTIONS` 启用时再列 `OPTIONS`（去重、字典序）；
  - **HEAD body 抑制放在分发/发送层**，不污染纯 Router。
- **框架生成响应也经全局 middleware**（避免旁路）：`RoutingHandler` 把 400/404/405/自动 OPTIONS
  同样交给 `MiddlewareChain::execute`（用合成 handler、`route_mws=nullptr` → **只全局、不跑路由业务 handler/路由级鉴权**），
  从而 CORS/request-id/access-log 等**全局 after 在错误响应与 OPTIONS 预检上也生效**；after 统一由 `run_after_chain` 执行一次。
  规则：Found=全局(+路由) middleware；框架 400/404/405/OPTIONS=仅全局 middleware；HEAD→GET 跑与 GET 相同的 middleware，仅发送层抑制实体。
- **参数生命周期**：`params` 为值语义、随 `MatchResult` 返回并独立拥有；DFS 回溯时对**未命中路径**的捕获做 `erase`；
  同一路径内**参数名唯一**（注册期强制），避免回溯 `erase` 误删同名捕获。
- **百分号解码边界**：`%` 后不足 2 位或非十六进制 → **400**；`%00` 解为内嵌 `\0`（`std::string` 承载，调用方需自行防注入）；
  路径分量中的 `+` **保持字面**（不转空格，那是 query 语义）。`//` 产生空段 → 注册期报错、匹配期 404。
- **handler 所有权**：trie 以 `unique_ptr` 持有节点、`std::function` 值持有 handler；`match` 返回 handler 的**拷贝**，
  故其可安全脱离 Router 生命周期（handler 自身捕获的外部引用由调用方负责）。

## 4. Middleware（提交3）—— after / finalize_once 契约（评审拍板，实现前冻结）

```cpp
using BeforeMiddleware = std::function<bool(HttpContext&)>;              // false = 短路(须已设响应)
enum class CompletionReason { Success, Error, ShortCircuited, Disconnected };
using AfterMiddleware  = std::function<void(HttpContext&, CompletionReason)>;
router.use(requestId); router.use(cors); router.use(accessLog);
route.use(authenticate).use(requireRole("customer"));
```

### 4.1 唯一完成出口（同步/异步共用）
所有路径**汇合到 `finalize_once`**，绝不各自发送：
```
before(全局→路由，顺序) → handler
        ├─ 同步返回 ──┐
        ├─ 异步成功 ──┤
        ├─ 异步异常 ──┤→ finalize_once(reason): 组装最终响应 → after(逆序) → 标记 Finalized → send
        ├─ 队列拒绝 ──┤
        └─ before 短路┘
```
- `AsyncPending` 期间**绝不提前跑 after**；只有真正 finalize 时才跑。
- 已进入的 **middleware 进入栈保存在 request-scoped Context**（随 `reset_for_request` 清空、受 `request_generation` 隔离）；换请求后旧栈作废。

### 4.2 after 的准确时机（关键）
**不是"响应已发送后"**，而是：
> **最终响应已确定、但网络字节尚未提交时**，逆序执行 after；after 跑完**才真正 send 并置 `Finalized`**。

如此 after 可安全**追加响应头 / 记录最终状态码 / 统一 CORS**。（实现：在 `finalize_once` 内、`do_notify_complete()` 之前跑 after 链。）

### 4.3 after 执行范围
- 只运行 **before 实际进入过**的 middleware（含**返回 false 的那个**）；
- 未进入的 middleware **不运行**；
- 全局级 + 路由级构成**一条进入栈**，统一**严格逆序**；
- 每个 after **最多执行一次**。

### 4.4 短路约定
`before == false` 表示：**该 middleware 已设置响应**，框架停止后续 before 与 handler，进入统一 finalize（reason=`ShortCircuited`）。
- 若短路时**未设置有效响应** → 框架**兜底 500**（不猜测 401/403）。

### 4.5 异常规则
- **before / handler 抛异常**（尚未 finalize）→ 转 **500**（reason=`Error`），随后跑**已进入的 after**；
- **某个 after 抛异常** → **记录并继续**跑剩余 after；**不再重写响应**、**不阻断 finalize**；
- **已 finalize 之后的任何异常** → **不得二次发送**。

### 4.6 连接断开 / 取消 —— after = 请求结束钩子（v1 决策已定）
after 定义为**请求结束钩子**（非"响应发送钩子"），以便计时/审计/资源释放在所有路径都不漏。
- 语义目标（长期）：断开/异步取消也应执行一次 after，`reason=Disconnected`。
- **v1（Commit 3）明确决策**：只落地 `Success / Error / ShortCircuited` 三条路径的 after（同步或经 `finalize_once`）。
  **断线/异步取消在 v1 不执行 after**——连接消失时仅在网络线程丢弃回投消息，Context 侧无触发。这是**明确的已知边界，无模糊行为**。
- `CompletionReason::Disconnected` 枚举值**保留**（不删），但 v1 不产生；"断开→finalize(Disconnected)"接线留作**独立后续提交**，
  届时补齐"断线只跑一次 after 且不二次发送"的集成测试。使用 after 做资源释放的业务在 v1 须知悉断线路径不回调。

### 4.7 其它
- v1 **不引入 `next()` 续延式**，避免异步生命周期复杂化。
- 框架**只**提供 chain / short-circuit / extensions / pre-post / 错误处理；不含任何业务鉴权。

**实现（提交3，`core/http_middleware.{h,cpp}` + Context 扩展）：**
- `Middleware{before?, after?}`；`MiddlewareChain::use/use_before/use_after`；
  `execute(ctx, handler, route_mws?)`：全局 + 路由级构成一条进入栈，进入即 `ctx.push_after(after)`。
- **after 单一执行点** `HttpContextImpl::run_after_chain(reason)`：逆序、`_after_ran` 保证最多一次、
  单个 after 抛异常 `PDEBUG` 记录并续跑、不改写响应；`reason==Disconnected` 时 v1 直接跳过。
- **同步/异步汇合**：`execute` 末尾若 `ctx.async_pending()` 为真则**不跑 after**（交 `finalize_once`）；
  否则同步就地 `run_after_chain`（在 `on_http_request` 返回、框架发送之前）。`finalize_once` 在
  `apply` 之后、`do_notify_complete()` 之前调用 `run_after_chain` —— 即"响应已定、字节未发"。
- **短路 500 兜底**：`has_valid_response` 以 pristine（status==200 且 body/headers 皆空）判定"未设响应"。
  **已知限制**：无法区分"裸 200 空响应短路"与"忘设响应"；后续以显式 `response_touched` 状态替代。
- request-scoped：`_after_stack`/`_after_ran` 随 `reset_for_request` 清空，受 `request_generation` 隔离；
  代号不匹配的陈旧异步回投在 `finalize_once` 即被丢弃，绝不运行当前请求的 after（已回归）。
- **before 必须同步**：`execute` 在每个 before 返回后检测 `async_pending()`；若 before 内发起了
  `async_response`（进入 AsyncPending），**记录并不再跑 handler**、立即返回（避免"已挂起却继续"的模糊态）。
- **after 仅对两阶段 async 与同步路径生效**：`do_notify_complete` 只经 `finalize_once`（两阶段异步）或
  幂等的 `complete_async_response`（**已弃用的单阶段 API / 裸流式**）触达；后者**不跑 after**——
  用中间件的业务须使用两阶段 `async_response(work, on_success)` 或同步返回。
- **业务层**（energyhub，属 Commit 4）实现 `authenticate`：解析 JWT → `ctx.set_ext<Principal>("principal", {...})`；`requireRole(r)` 读 `get_ext<Principal>`。
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

**响应侧 JSON helper —— 最小实现约束（评审拍板）：**
- **自动设置 `Content-Type: application/json`**（可被显式覆盖）；
- body 由 **`HttpResponse` 值持有**（拷贝/移动进 `_send_body` 前的 `response().body`），
  **绝不引用临时 buffer**（避免异步回投后悬空）；
- **不把 RapidJSON / JWT 类型引入公共接口**：`set_json` 对外只接受 `std::string`（已序列化）或框架内部包装，
  第三方 JSON 库仅存在于 .cpp / 业务层；
- **序列化失败**（如传入非法 UTF-8 / 编码异常）→ 进入**统一 500 路径**（经 finalize_once），不产出半个响应。

**实现（提交3，`core/http_json.h`，仅响应侧、header-only）：**
- `HttpResponse::set_json(std::string)` 既有（自动 `application/json; charset=utf-8`、body 值持有）；
- `myframe::json_error(res, code, msg)`：`{"error":"<escaped>"}` + 状态码 + CT；`json_escape` 处理引号/反斜杠/控制字符；
- **不含解析**：`parse_json` 属业务层（避免把 RapidJSON 带进框架公共接口）；序列化由业务完成，失败自行走 500。

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
