# myframe HTTP 改进路线（Roadmap）

> 目标：先修地基（Context 生命周期 / 异步安全 / 类型安全扩展），再加 Router + Middleware + JSON 便捷。
> 原则：**分阶段独立提交**，Router 与业务改造不混在一个提交里。JWT/租户等业务概念**不进框架核心**。

## 背景（e2e 中暴露的真实问题）

- header 值未去 OWS 前导空格（已修：http_res_process.cpp + get_header 大小写不敏感）。
- `HttpContext::set_user_data(key, void*)`：不拥有对象、无类型安全、Keep-Alive 下**可能跨请求泄漏上一次租户身份**（多租户 PAYG 下是安全问题）。
- `HttpContext::async_response(fn)`：detached thread 捕获裸 `this`，连接提前关闭有悬空风险；每请求建线程。

## 阶段与提交（5 个独立提交）

0. **fix(http): normalize header OWS and support case-insensitive lookup**
   - `http_res_process.cpp`：去 header 值冒号后 OWS 前导/尾随空格。
   - `app_handler_v2.h`：`get_header` 大小写不敏感（RFC7230）。
   - 独立完整 bug fix；不并入 Context 提交；无关改动（如 web_socket_process.cpp）不混入。

1. **fix(http): make request context and async lifetime safe**
   - 明确 Context 语义（请求级视图；连接复用时每请求 reset）。
   - 每请求重置：response / route params / extensions。
   - 新增**类型安全 request extensions**（拥有对象），鉴权场景弃用裸 `void*`。
   - `async_response` 改为**线程池执行** + 完成回调经 `HttpContextTaskMessage`/`send_msg` **投递回所属网络线程**；不跨线程持有裸 `this`；连接关闭则取消/忽略完成回调。
   - 验收：Keep-Alive 连续两请求，extensions 不串；异步任务在连接关闭后不崩、不触发已释放对象。

2. **feat(http): add router and path parameters**
   - `router.get/post/...("/api/v1/devices/{id}", handler)`。
   - 见 `http-router-rfc.md` 的匹配规则（静态优先、{}捕获、405+Allow、启动期冲突报错、注册后冻结只读）。
   - 不含业务；handler 签名用现有 `HttpContext&`。

3. **feat(http): add middleware and JSON helpers**
   - `router.use(mw)` 全局 + 每路由 `.use(mw)`；pre/post + short-circuit + 统一错误处理。
   - JSON：`ctx.request().parse_json()` 区分解析失败；大小写/大小限制/UTF-8/标准错误响应/自动 Content-Type。
   - 框架**只**给 chain/extensions，不含 jwtAuth/Principal。

4. **refactor(energyhub): migrate customer and ops handlers**
   - 用 Router + Middleware 重写 energyhub-customer-api / energyhub-ops-api 的 handler。
   - 目的：对比前后代码量，检验框架接口是否真的好用。

## 暂缓

- ⑤ DB 连接池 / 异步 DbClient / ORM：工程量大，myframe 定位是网络框架，DB 继续用 libpqxx（顶多加连接池小工具），本轮不做。

## 文档

- 本文件：路线与阶段。
- `http-router-rfc.md`：Context/异步/Router/Middleware/JSON 的接口设计。
- **CHANGELOG.md 只记已完成发布的变更**，每个提交合入后再补对应条目。
