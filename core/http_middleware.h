// http_middleware.h · 中间件链（提交3）
// 契约见 docs/http-router-rfc.md §4。框架只提供 chain / short-circuit / pre-post / 错误处理，
// 不含任何业务鉴权。与 finalize_once 共用唯一完成出口：
//   before(顺序) → handler → [同步: run_after_chain] / [异步: finalize_once 内 run_after_chain]
// after：请求结束钩子，逆序、最多一次、单个抛异常不阻断。
#ifndef __MYFRAME_HTTP_MIDDLEWARE_H__
#define __MYFRAME_HTTP_MIDDLEWARE_H__

#include "protocol_context.h"     // Middleware / BeforeMiddleware / AfterMiddleware
#include "http_router.h"          // RouteHandler
#include <vector>

namespace myframe {

class MiddlewareChain {
public:
    MiddlewareChain() {}

    MiddlewareChain& use(Middleware mw) { _mws.push_back(std::move(mw)); return *this; }
    MiddlewareChain& use_before(BeforeMiddleware b) { return use(Middleware(std::move(b), AfterMiddleware())); }
    MiddlewareChain& use_after(AfterMiddleware a)   { return use(Middleware(BeforeMiddleware(), std::move(a))); }

    // 执行一次请求：全局中间件 + 可选路由级中间件（追加在后，进入顺序在全局之后）。
    //   - 进入某中间件即登记其 after（含短路那个）到 Context；
    //   - 任一 before 返回 false → 短路（未设响应则兜底 500）；
    //   - before/handler 抛异常 → 500；
    //   - 异步：after 交由 finalize_once 执行；同步：此处在返回前逆序执行 after。
    void execute(HttpContext& ctx, const RouteHandler& handler,
                 const std::vector<Middleware>* route_mws = 0) const;

    size_t size() const { return _mws.size(); }

private:
    static bool has_valid_response(HttpContext& ctx);   // 短路是否已设有效响应
    static void set_500(HttpContext& ctx);

    std::vector<Middleware> _mws;   // 全局中间件（进入顺序）
};

} // namespace myframe

#endif
