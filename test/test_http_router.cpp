// test_http_router.cpp · 路由表回归（提交2）
// 覆盖：静态优先 / {param} 冲突 / %2F 段内字面 / 非法编码 400 / query 不入参 /
//       尾斜杠等价 / 405 Allow 去重有序 / freeze 后拒绝注册 / 并发只读匹配 / handler 身份。
#include "http_router.h"
#include "protocol_adapters/http_context_adapter.h"   // 用真实 HttpContextImpl 调用 handler 验证身份
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace myframe;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// handler：把唯一 tag 写进 response.status，便于断言“命中的是哪条路由”
static RouteHandler mk(int tag) {
    return RouteHandler([tag](HttpContext& c) { c.response().status = tag; });
}
static int invoke(const RouteHandler& h) {
    HttpContextImpl ctx(nullptr, nullptr);
    h(ctx);
    return ctx.response().status;
}

int main() {
    // ---- 1) 静态段优先于 {param}，且命中正确 handler ----
    {
        Router r;
        r.get("/devices/{id}", mk(1));
        r.get("/devices/status", mk(2));      // 注册顺序故意后于 {id}
        r.freeze();

        MatchResult a = r.match("GET", "/devices/status");
        CHECK(a.status == MatchStatus::Found);
        CHECK(a.params.empty());              // 静态命中，无捕获
        CHECK(invoke(a.handler) == 2);        // 命中静态路由

        MatchResult b = r.match("GET", "/devices/123");
        CHECK(b.status == MatchStatus::Found);
        CHECK(b.params.size() == 1 && b.params["id"] == "123");
        CHECK(invoke(b.handler) == 1);

        // 静态前缀走进死路时回溯到参数段：/a/b/c 命中 /a/{p}/c
        Router r2;
        r2.get("/a/{p}/c", mk(10));
        r2.get("/a/b/d", mk(11));
        r2.freeze();
        MatchResult c = r2.match("GET", "/a/b/c");
        CHECK(c.status == MatchStatus::Found && c.params["p"] == "b" && invoke(c.handler) == 10);
    }

    // ---- 2) {id} 与 {name}：同位不同名 → 注册期冲突 ----
    {
        Router r;
        r.get("/devices/{id}", mk(1));
        bool threw = false;
        try { r.get("/devices/{name}", mk(2)); }
        catch (const std::exception&) { threw = true; }
        CHECK(threw);
        // 同名不冲突
        Router r2;
        r2.get("/devices/{id}", mk(1));
        bool threw2 = false;
        try { r2.get("/devices/{id}/detail", mk(2)); }
        catch (const std::exception&) { threw2 = true; }
        CHECK(!threw2);
        // 重复路由（同 method 同 path）→ 冲突
        Router r3;
        r3.get("/x", mk(1));
        bool threw3 = false;
        try { r3.get("/x", mk(2)); } catch (const std::exception&) { threw3 = true; }
        CHECK(threw3);
        // 同一路径内参数重名 → 冲突（否则回溯 erase 会误删同名捕获）
        Router r4;
        bool threw4 = false;
        try { r4.get("/{id}/{id}", mk(1)); } catch (const std::exception&) { threw4 = true; }
        CHECK(threw4);
        // 空段（'//'）→ 注册期报错
        Router r5;
        bool threw5 = false;
        try { r5.get("/a//b", mk(1)); } catch (const std::exception&) { threw5 = true; }
        CHECK(threw5);
    }

    // ---- 3) %2F 不在分段前解码：保持段内字面，不改变层级 ----
    {
        Router r;
        r.get("/files/{p}", mk(1));    // 单段参数
        r.get("/x/y", mk(2));          // 两个静态段
        r.freeze();

        MatchResult a = r.match("GET", "/files/a%2Fb");
        CHECK(a.status == MatchStatus::Found);
        CHECK(a.params["p"] == "a/b");          // %2F 解码为 '/' 但仍属同一段
        CHECK(invoke(a.handler) == 1);

        MatchResult b = r.match("GET", "/x%2Fy");  // 单段 "x/y"，不等于 /x/y
        CHECK(b.status == MatchStatus::NotFound);
    }

    // ---- 4) 非法百分号编码 → 400 ----
    {
        Router r;
        r.get("/a/{p}", mk(1));
        r.freeze();
        CHECK(r.match("GET", "/a/%zz").status == MatchStatus::BadRequest);   // 非十六进制
        CHECK(r.match("GET", "/a/%4").status  == MatchStatus::BadRequest);   // 截断
        CHECK(r.match("GET", "/a/%20").params.empty() == false);            // 合法：%20 → 空格
        CHECK(r.match("GET", "/a/%20").params["p"] == " ");
    }

    // ---- 5) query 不参与匹配、不进入参数值 ----
    {
        Router r;
        r.get("/devices/{id}", mk(1));
        r.freeze();
        MatchResult a = r.match("GET", "/devices/123?foo=bar&x=1");
        CHECK(a.status == MatchStatus::Found);
        CHECK(a.params["id"] == "123");         // 不含 ?foo=bar
    }

    // ---- 6) /x 与 /x/ 等价（注册与匹配都规范化尾斜杠）----
    {
        Router r;
        r.get("/x/", mk(1));                    // 注册带尾斜杠
        r.freeze();
        CHECK(r.match("GET", "/x").status == MatchStatus::Found);
        CHECK(r.match("GET", "/x/").status == MatchStatus::Found);

        Router r2;
        r2.get("/y", mk(2));                    // 注册不带尾斜杠
        r2.freeze();
        CHECK(r2.match("GET", "/y/").status == MatchStatus::Found);
    }

    // ---- 7) 405 + Allow：去重且字典序稳定 ----
    {
        Router r;
        r.post("/r", mk(1));
        r.get("/r", mk(2));
        r.del("/r", mk(3));
        r.freeze();
        MatchResult a = r.match("PATCH", "/r");
        CHECK(a.status == MatchStatus::MethodNotAllowed);
        std::vector<std::string> expect;
        expect.push_back("DELETE"); expect.push_back("GET"); expect.push_back("POST");
        CHECK(a.allow == expect);               // 字典序、去重、稳定
    }

    // ---- 8) freeze 后拒绝注册 ----
    {
        Router r;
        r.get("/a", mk(1));
        r.freeze();
        bool threw = false;
        try { r.get("/b", mk(2)); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
        CHECK(r.frozen());
    }

    // ---- 9) 并发只读匹配：freeze 后多线程 match 结果正确、无崩溃 ----
    {
        Router r;
        r.get("/devices/status", mk(2));
        r.get("/devices/{id}", mk(1));
        r.get("/a/{p}/c", mk(10));
        r.freeze();

        std::atomic<int> bad(0);
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; ++t) {
            ts.emplace_back([&r, &bad]() {
                for (int i = 0; i < 5000; ++i) {
                    MatchResult a = r.match("GET", "/devices/status");
                    if (!(a.status == MatchStatus::Found && a.params.empty())) ++bad;
                    MatchResult b = r.match("GET", "/devices/42");
                    if (!(b.status == MatchStatus::Found && b.params["id"] == "42")) ++bad;
                    MatchResult c = r.match("GET", "/a/xy/c");
                    if (!(c.status == MatchStatus::Found && c.params["p"] == "xy")) ++bad;
                    MatchResult d = r.match("GET", "/nope");
                    if (d.status != MatchStatus::NotFound) ++bad;
                }
            });
        }
        for (auto& th : ts) th.join();
        CHECK(bad.load() == 0);
    }

    // ---- 附：404（路径无匹配）----
    {
        Router r;
        r.get("/a", mk(1));
        r.freeze();
        CHECK(r.match("GET", "/b").status == MatchStatus::NotFound);
    }

    if (g_fail == 0) { printf("test_http_router: ALL PASS\n"); return 0; }
    printf("test_http_router: %d FAIL\n", g_fail);
    return 1;
}
