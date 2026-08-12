// test_http_middleware.cpp · 中间件链回归（提交3）
// 覆盖 docs/http-router-rfc.md §4：before 顺序 / after 逆序 / 进入栈范围 / 短路(含兜底500) /
//     异常→500后仍跑已进入 after / after 抛异常续跑不改写 / after 可加头 / 每个最多一次。
#include "http_middleware.h"
#include "protocol_adapters/http_context_adapter.h"
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace myframe;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// 记录进入顺序的中间件对：before 追加小写、after 追加大写。
static Middleware trace_mw(std::string& log, char c, bool before_ok = true) {
    char lo = c, hi = (char)(c - 'a' + 'A');
    BeforeMiddleware b = [&log, lo, before_ok](HttpContext&) -> bool {
        log.push_back(lo); return before_ok;
    };
    AfterMiddleware a = [&log, hi](HttpContext&, CompletionReason) { log.push_back(hi); };
    return Middleware(b, a);
}

int main() {
    // ---- 1) before 顺序 + handler + after 逆序；reason=Success ----
    {
        MiddlewareChain chain;
        std::string log;
        CompletionReason seen = CompletionReason::Disconnected;
        chain.use(trace_mw(log, 'a'));
        chain.use(trace_mw(log, 'b'));
        chain.use(Middleware(BeforeMiddleware(),
                             [&log, &seen](HttpContext&, CompletionReason r) { log.push_back('C'); seen = r; }));
        RouteHandler h = [&log](HttpContext&) { log.push_back('H'); };

        HttpContextImpl ctx(nullptr, nullptr);
        chain.execute(ctx, h);
        CHECK(log == "abHCBA");                       // before a,b → H → after C,B,A（逆序）
        CHECK(seen == CompletionReason::Success);
        CHECK(ctx.response().status == 200);
    }

    // ---- 2) 短路：停止后续 before 与 handler；已进入 after（含短路那个）逆序执行 ----
    {
        MiddlewareChain chain;
        std::string log;
        chain.use(trace_mw(log, 'a'));                // 进入
        // b：before 返回 false 且设置有效响应（403）
        chain.use(Middleware(
            [&log](HttpContext& c) -> bool { log.push_back('b'); c.response().status = 403; return false; },
            [&log](HttpContext&, CompletionReason) { log.push_back('B'); }));
        chain.use(trace_mw(log, 'c'));                // 不应进入
        RouteHandler h = [&log](HttpContext&) { log.push_back('H'); };

        HttpContextImpl ctx(nullptr, nullptr);
        chain.execute(ctx, h);
        CHECK(log == "abBA");                          // a,b 进入；无 H；after 只跑 B,A（c 未进入）
        CHECK(ctx.response().status == 403);           // 短路已设响应，不被 500 覆盖
    }

    // ---- 3) 短路但未设响应 → 框架兜底 500 ----
    {
        MiddlewareChain chain;
        chain.use(Middleware([](HttpContext&) -> bool { return false; }, AfterMiddleware()));
        RouteHandler h = [](HttpContext&) {};
        HttpContextImpl ctx(nullptr, nullptr);
        chain.execute(ctx, h);
        CHECK(ctx.response().status == 500);
    }

    // ---- 4) handler 抛异常 → 500；已进入 after 仍逆序执行，reason=Error ----
    {
        MiddlewareChain chain;
        std::string log;
        CompletionReason seen = CompletionReason::Success;
        chain.use(Middleware(
            [&log](HttpContext&) -> bool { log.push_back('a'); return true; },
            [&log, &seen](HttpContext&, CompletionReason r) { log.push_back('A'); seen = r; }));
        RouteHandler h = [](HttpContext&) { throw std::runtime_error("boom"); };
        HttpContextImpl ctx(nullptr, nullptr);
        chain.execute(ctx, h);
        CHECK(ctx.response().status == 500);
        CHECK(log == "aA");
        CHECK(seen == CompletionReason::Error);
    }

    // ---- 5) 某个 after 抛异常 → 记录并续跑剩余 after；不改写响应；不阻断 ----
    {
        MiddlewareChain chain;
        std::string log;
        chain.use(Middleware(BeforeMiddleware(),
                             [&log](HttpContext& c, CompletionReason) { log.push_back('A'); c.response().status = 222; }));  // 外层 after
        chain.use(Middleware(BeforeMiddleware(),
                             [](HttpContext&, CompletionReason) { throw std::runtime_error("after-boom"); })); // 内层 after 抛
        RouteHandler h = [](HttpContext&) {};
        HttpContextImpl ctx(nullptr, nullptr);
        ctx.response().status = 200;
        chain.execute(ctx, h);                         // 内层 after 先跑（逆序）抛异常，外层 after 仍跑
        CHECK(log == "A");                             // 外层 after 执行了
        CHECK(ctx.response().status == 222);           // 响应未被“抛异常的 after”改写；外层设置生效
    }

    // ---- 6) after 可安全追加响应头（发送前）；7) 每个 after 最多一次 ----
    {
        MiddlewareChain chain;
        int a_count = 0;
        chain.use(Middleware(BeforeMiddleware(),
            [&a_count](HttpContext& c, CompletionReason) { ++a_count; c.response().set_header("X-Trace", "1"); }));
        RouteHandler h = [](HttpContext& c) { c.response().set_text("ok"); };
        HttpContextImpl ctx(nullptr, nullptr);
        chain.execute(ctx, h);
        CHECK(ctx.response().headers.count("X-Trace") == 1 && ctx.response().headers["X-Trace"] == "1");
        CHECK(a_count == 1);
        // 再次显式调用 run_after_chain 不应重复执行（最多一次）
        ctx.run_after_chain(CompletionReason::Success);
        CHECK(a_count == 1);
    }

    if (g_fail == 0) { printf("test_http_middleware: ALL PASS\n"); return 0; }
    printf("test_http_middleware: %d FAIL\n", g_fail);
    return 1;
}
