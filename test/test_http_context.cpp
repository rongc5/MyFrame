// test_http_context.cpp · 回归测试：HttpContext 请求级安全
//   - 类型安全 extensions（set_ext/get_ext），类型不符/不存在返回 null
//   - reset_for_request 清空 extensions（杜绝 Keep-Alive 跨请求租户身份泄漏）
//   - reset_for_request 清空 response、++request_generation
// 对应：fix(http): make request context and async lifetime safe（1a Context 安全）
#include "protocol_adapters/http_context_adapter.h"
#include <cstdio>
#include <string>
#include <stdexcept>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

struct Principal { long tenant_id; std::string role; };

int main() {
    myframe::HttpContextImpl ctx(nullptr, nullptr);

    CHECK(ctx.request_generation() == 0);

    // 类型安全扩展
    Principal& p = ctx.set_ext<Principal>("principal", Principal{42, "owner"});
    CHECK(p.tenant_id == 42);
    Principal* g = ctx.get_ext<Principal>("principal");
    CHECK(g != NULL && g->tenant_id == 42 && g->role == "owner");
    CHECK(ctx.get_ext<int>("principal") == NULL);     // 类型不符
    CHECK(ctx.get_ext<Principal>("nope") == NULL);    // 不存在

    // 模拟上一请求写了 response
    ctx.response().status = 404;
    ctx.response().body = "old-body";

    // Keep-Alive 下一请求开始
    ctx.reset_for_request();

    CHECK(ctx.request_generation() == 1);                       // 代号自增
    CHECK(ctx.get_ext<Principal>("principal") == NULL);         // 扩展清空 → 无跨请求泄漏
    CHECK(ctx.response().status == 200);                        // response 复位为默认
    CHECK(ctx.response().body.empty());

    // ===== finalize_once：网络线程统一完成出口的三重不变式 =====
    // (1) 幂等：同一代号只应用一次（对应“complete_async_response 重复调用只发送一次”）
    {
        myframe::HttpContextImpl c(nullptr, nullptr);   // gen=0, Handling
        int applied = 0;
        std::function<void(myframe::HttpContext&)> inc =
            [&applied](myframe::HttpContext&) { ++applied; };
        c.finalize_once(0, inc);
        c.finalize_once(0, inc);          // 已 Finalized → 幂等
        CHECK(applied == 1);
    }
    // (2) 代号变化 → 丢弃陈旧异步结果（Keep-Alive 已进入下一请求）
    {
        myframe::HttpContextImpl c(nullptr, nullptr);   // gen=0
        c.reset_for_request();                          // gen=1, Handling
        int applied = 0;
        std::function<void(myframe::HttpContext&)> inc =
            [&applied](myframe::HttpContext&) { ++applied; };
        c.finalize_once(0, inc);          // 旧代号 → 丢弃
        CHECK(applied == 0);
        c.finalize_once(1, inc);          // 新代号 → 应用
        CHECK(applied == 1);
    }
    // (3) apply 抛异常（work 异常 rethrow 或 on_success 抛）→ 兜底 500，请求仍完成
    {
        myframe::HttpContextImpl c(nullptr, nullptr);
        c.finalize_once(0, [](myframe::HttpContext&) { throw std::runtime_error("boom"); });
        CHECK(c.response().status == 500);
        c.finalize_once(0, [](myframe::HttpContext& x) { x.response().status = 200; }); // 已完成 → 幂等
        CHECK(c.response().status == 500);
    }
    // (4) 请求级 body 复位：reset 后 request().body 为空（防 Keep-Alive 跨请求泄漏）
    {
        myframe::HttpContextImpl c(nullptr, nullptr);
        c.mutable_request().body = "secret";
        c.reset_for_request();
        CHECK(c.request().body.empty());
    }
    // (5) 代号不匹配的异步回投不得运行“当前请求”的 after（gen 隔离）
    {
        myframe::HttpContextImpl c(nullptr, nullptr);   // gen=0（请求A）
        c.reset_for_request();                          // gen=1（请求B）
        int b_after = 0;
        c.push_after([&b_after](myframe::HttpContext&, myframe::CompletionReason) { ++b_after; });
        std::function<void(myframe::HttpContext&)> none;
        c.finalize_once(0, none, myframe::CompletionReason::Success);   // A 的陈旧回投(gen=0)
        CHECK(b_after == 0);                            // 不得运行 B 的 after
        c.finalize_once(1, none, myframe::CompletionReason::Success);   // B 正常完成
        CHECK(b_after == 1);                            // 恰好一次
    }

    if (g_fail == 0) { printf("test_http_context: ALL PASS\n"); return 0; }
    printf("test_http_context: %d FAIL\n", g_fail);
    return 1;
}
