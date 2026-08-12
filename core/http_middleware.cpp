// http_middleware.cpp · 中间件链实现（提交3）
#include "http_middleware.h"

namespace myframe {

bool MiddlewareChain::has_valid_response(HttpContext& ctx) {
    // 短路时判断中间件是否已设置有效响应：默认 pristine（200/空 body/空头）视为“未设置”。
    const HttpResponse& r = ctx.response();
    if (r.status != 200) return true;
    if (!r.body.empty()) return true;
    if (!r.headers.empty()) return true;
    return false;
}

void MiddlewareChain::set_500(HttpContext& ctx) {
    ctx.response() = HttpResponse();
    ctx.response().status = 500;
    ctx.response().reason = "Internal Server Error";
}

void MiddlewareChain::execute(HttpContext& ctx, const RouteHandler& handler,
                              const std::vector<Middleware>* route_mws) const {
    bool threw = false;
    bool short_circuit = false;

    // 单条进入栈：全局在前、路由级在后，严格顺序进入。
    // 进入某中间件即登记其 after（含短路那个）；未进入的不登记 → after 逆序时只跑进入过的。
    const std::vector<Middleware>* lists[2] = { &_mws, route_mws };
    for (int li = 0; li < 2 && !threw && !short_circuit; ++li) {
        const std::vector<Middleware>* list = lists[li];
        if (!list) continue;
        for (size_t i = 0; i < list->size(); ++i) {
            const Middleware& mw = (*list)[i];
            if (mw.after) ctx.push_after(mw.after);      // 进入即登记 after
            if (mw.before) {
                bool ok = true;
                try { ok = mw.before(ctx); }
                catch (...) { threw = true; break; }
                // before 必须同步：若其内发起了 async_response（进入 AsyncPending），
                // 继续跑 handler 会造成“已挂起却继续”的模糊态。明确检测 → 记录并**不跑 handler**、
                // 立即返回；收尾交由该 async 的 finalize_once（afters 已登记，仍会执行一次）。
                if (ctx.async_pending()) {
                    PDEBUG("[MiddlewareChain] before must be synchronous (it called async_response); skipping handler");
                    return;
                }
                if (!ok) { short_circuit = true; break; }
            }
        }
    }

    if (!threw && !short_circuit) {
        try { handler(ctx); }
        catch (...) { threw = true; }
    }

    CompletionReason reason = CompletionReason::Success;
    if (threw) {
        set_500(ctx);
        reason = CompletionReason::Error;
    } else if (short_circuit) {
        reason = CompletionReason::ShortCircuited;
        if (!has_valid_response(ctx)) set_500(ctx);      // 短路未设响应 → 兜底 500（不猜 401/403）
    }

    // 异步：handler 已进入 AsyncPending，after 交由 finalize_once 在真正完成时执行。
    if (ctx.async_pending()) return;
    // 同步：响应已定、字节未发 → 此刻逆序执行 after（发送在 execute 返回后由框架完成）。
    ctx.run_after_chain(reason);
}

} // namespace myframe
