// test_http_context.cpp · 回归测试：HttpContext 请求级安全
//   - 类型安全 extensions（set_ext/get_ext），类型不符/不存在返回 null
//   - reset_for_request 清空 extensions（杜绝 Keep-Alive 跨请求租户身份泄漏）
//   - reset_for_request 清空 response、++request_generation
// 对应：fix(http): make request context and async lifetime safe（1a Context 安全）
#include "protocol_adapters/http_context_adapter.h"
#include <cstdio>
#include <string>

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

    if (g_fail == 0) { printf("test_http_context: ALL PASS\n"); return 0; }
    printf("test_http_context: %d FAIL\n", g_fail);
    return 1;
}
