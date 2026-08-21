// test_http_routing.cpp · Router 接入分发集成测试（Commit 4 / 子提交1）
//   真实 Level 2 服务 + RoutingHandler：route_param 注入 / 静态优先 /
//   HEAD→GET(抑制 body,保留 Content-Length) / auto-OPTIONS 204+Allow / 404 / 405+Allow / 400 /
//   中间件 after 经分发生效。
#include "server.h"
#include "unified_protocol_factory.h"
#include "http_routing_handler.h"
#include "http_json.h"
#include "protocol_adapters/http_context_adapter.h"   // 单元级：直接调 on_http_request

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// ---- 极简 socket 客户端 ----
static int dial(unsigned short port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a; std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (::connect(fd, (sockaddr*)&a, sizeof(a)) != 0) { ::close(fd); return -1; }
    return fd;
}
static void send_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) { ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0); if (n <= 0) break; off += (size_t)n; }
}
static int status_of(const std::string& resp) {
    size_t sp = resp.find(' '); return sp == std::string::npos ? -1 : std::atoi(resp.c_str() + sp + 1);
}
// 读完整响应（含 body，按 Content-Length 定帧）。
static std::string read_full(int fd, std::string& body) {
    std::string buf; size_t he;
    while ((he = buf.find("\r\n\r\n")) == std::string::npos) {
        char t[4096]; ssize_t n = ::recv(fd, t, sizeof(t), 0); if (n <= 0) return std::string(); buf.append(t, (size_t)n);
    }
    size_t cl = 0, p = buf.find("Content-Length:");
    if (p != std::string::npos) cl = (size_t)std::atol(buf.c_str() + p + 15);
    size_t bs = he + 4;
    while (buf.size() - bs < cl) { char t[4096]; ssize_t n = ::recv(fd, t, sizeof(t), 0); if (n <= 0) break; buf.append(t, (size_t)n); }
    body = buf.substr(bs, cl);
    return buf;
}
// 只读 header 块（HEAD 用：不读 body，即使有 Content-Length）。
static std::string read_headers(int fd) {
    std::string buf; size_t he;
    while ((he = buf.find("\r\n\r\n")) == std::string::npos) {
        char t[4096]; ssize_t n = ::recv(fd, t, sizeof(t), 0); if (n <= 0) return buf; buf.append(t, (size_t)n);
    }
    return buf.substr(0, he + 4);
}
static int req(unsigned short port, const std::string& method, const std::string& path,
               std::string* body = 0, std::string* head = 0, bool head_only = false) {
    int fd = dial(port); if (fd < 0) return -1;
    send_all(fd, method + " " + path + " HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    std::string resp, b;
    if (head_only) { resp = read_headers(fd); }
    else { resp = read_full(fd, b); if (body) *body = b; }
    if (head) *head = resp;
    ::close(fd);
    return resp.empty() ? -1 : status_of(resp);
}

int main(int argc, char** argv) {
    using myframe::HttpContext;
    using myframe::HttpContextImpl;
    using myframe::Router;
    using myframe::MiddlewareChain;
    using myframe::Middleware;
    using myframe::BeforeMiddleware;
    using myframe::AfterMiddleware;
    using myframe::CompletionReason;
    using myframe::RoutingHandler;
    using myframe::RoutingOptions;

    // ===== 单元级：路由级 middleware 真正进入 MiddlewareChain（短路语义 + after-once）=====
    // 证明 route 级 before 短路时：reason=ShortCircuited、handler 不跑、route+global after 逆序各一次。
    {
        Router r;
        std::string log;
        r.get("/g", [&log](HttpContext& c) { log += "H"; c.response().set_text("ok"); })
            .use(Middleware(
                [&log](HttpContext& c) -> bool {
                    log += "a";
                    if (c.request().get_header("X-Auth") != "y") { c.response().status = 401; return false; }
                    return true;
                },
                [&log](HttpContext&, CompletionReason) { log += "A"; }));   // 路由级 after
        r.freeze();

        MiddlewareChain chain;
        CompletionReason greason = CompletionReason::Success;
        chain.use(Middleware(BeforeMiddleware(),
            [&log, &greason](HttpContext&, CompletionReason rr) { log += "G"; greason = rr; }));  // 全局 after
        RoutingHandler h(r, chain);

        // 无 X-Auth → 路由 before 短路 401；handler 不跑；after 逆序 route(A) 先于 global(G)；reason=ShortCircuited
        {
            HttpContextImpl ctx(0, 0);
            ctx.mutable_request().method = "GET"; ctx.mutable_request().url = "/g";
            log.clear(); h.on_http_request(ctx);
            CHECK(ctx.response().status == 401);
            CHECK(log == "aAG");
            CHECK(greason == CompletionReason::ShortCircuited);
        }
        // 有 X-Auth → 通过：before a → handler H → after A,G；reason=Success
        {
            HttpContextImpl ctx(0, 0);
            ctx.mutable_request().method = "GET"; ctx.mutable_request().url = "/g";
            ctx.mutable_request().headers["X-Auth"] = "y";
            log.clear(); h.on_http_request(ctx);
            CHECK(ctx.response().status == 200);
            CHECK(log == "aHAG");
            CHECK(greason == CompletionReason::Success);
        }
    }
    // ===== 单元级：render_error 可配置（业务覆盖 404 文案，不改框架默认）=====
    {
        Router r; r.get("/x", [](HttpContext& c) { c.response().set_text("x"); }); r.freeze();
        MiddlewareChain chain;
        RoutingOptions opt;
        opt.render_error = [](HttpContext& c, int st, const std::string& code) {
            if (code == "not_found") myframe::json_error(c.response(), 404, "not found");
            else myframe::json_error(c.response(), st, code);
        };
        RoutingHandler h(r, chain, opt);
        HttpContextImpl ctx(0, 0);
        ctx.mutable_request().method = "GET"; ctx.mutable_request().url = "/nope";
        h.on_http_request(ctx);
        CHECK(ctx.response().status == 404);
        CHECK(ctx.response().body == "{\"error\":\"not found\"}");   // 业务覆盖生效
    }

    unsigned short port = (argc > 1) ? (unsigned short)std::atoi(argv[1]) : 18077;

    myframe::Router router;
    router.get("/devices/status", [](myframe::HttpContext& c) { c.response().set_text("STATUS"); });
    router.get("/devices/{id}", [](myframe::HttpContext& c) {
        c.response().set_text("id=" + c.route_param("id"));
    });
    // 显式 HEAD/OPTIONS：应优先于自动合成
    router.add("OPTIONS", "/explicit", [](myframe::HttpContext& c) {
        c.response().status = 200; c.response().set_text("EXPLICIT-OPT");
    });
    router.add("HEAD", "/hexp", [](myframe::HttpContext& c) {
        c.response().status = 200; c.response().set_header("X-Head", "explicit");
    });
    router.freeze();

    myframe::MiddlewareChain chain;
    chain.use_after([](myframe::HttpContext& c, myframe::CompletionReason) {
        c.response().set_header("X-After", "1");
    });

    myframe::RoutingHandler handler(router, chain);   // auto_head + auto_options 默认开
    auto factory = std::make_shared<myframe::UnifiedProtocolFactory>();
    factory->register_http_context_handler(&handler);

    ::server srv(2);
    srv.bind("127.0.0.1", port);
    srv.set_business_factory(factory);
    srv.start();
    for (int i = 0; i < 100; ++i) { int fd = dial(port); if (fd >= 0) { ::close(fd); break; } std::this_thread::sleep_for(std::chrono::milliseconds(20)); }

    // route_param 注入 + 静态优先
    {
        std::string b, h;
        CHECK(req(port, "GET", "/devices/123", &b, &h) == 200);
        CHECK(b == "id=123");
        CHECK(h.find("X-After: 1") != std::string::npos);        // 中间件经分发生效
        std::string b2;
        CHECK(req(port, "GET", "/devices/status", &b2) == 200);
        CHECK(b2 == "STATUS");                                    // 静态优先
    }
    // HEAD→GET：抑制 body，但保留 Content-Length（= GET body 长度）
    {
        int fd = dial(port);
        CHECK(fd >= 0);
        send_all(fd, "HEAD /devices/123 HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        // 排空到 EOF（Connection: close）——可靠证明“是否真发了 body 字节”
        std::string buf; char t[4096]; ssize_t n;
        while ((n = ::recv(fd, t, sizeof(t), 0)) > 0) buf.append(t, (size_t)n);
        ::close(fd);
        size_t he = buf.find("\r\n\r\n");
        CHECK(he != std::string::npos);
        std::string head = buf.substr(0, he);
        std::string body = buf.substr(he + 4);
        CHECK(status_of(buf) == 200);
        CHECK(head.find("Content-Length: 6") != std::string::npos);  // Content-Length = GET body 长度
        CHECK(head.find("X-After: 1") != std::string::npos);         // headers 保留
        CHECK(body.empty());                                         // 关键：实际未发送任何 body 字节
    }
    // auto-OPTIONS：204 + Allow(GET,HEAD,OPTIONS)；且经全局 after（X-After）
    {
        std::string h, b;
        int st = req(port, "OPTIONS", "/devices/123", &b, &h);
        CHECK(st == 204);
        CHECK(h.find("Allow:") != std::string::npos);
        CHECK(h.find("GET") != std::string::npos && h.find("HEAD") != std::string::npos && h.find("OPTIONS") != std::string::npos);
        CHECK(h.find("X-After: 1") != std::string::npos);        // 框架 OPTIONS 也经全局 middleware
    }
    // 405 + Allow（POST 未注册）；且经全局 after
    {
        std::string h, b;
        int st = req(port, "POST", "/devices/status", &b, &h);
        CHECK(st == 405);
        CHECK(h.find("Allow:") != std::string::npos && h.find("GET") != std::string::npos && h.find("HEAD") != std::string::npos);
        CHECK(b == "{\"error\":\"method_not_allowed\"}");
        CHECK(h.find("X-After: 1") != std::string::npos);
    }
    // 404 / 400；框架错误响应也经全局 after
    {
        std::string h4, b4, h0, b0;
        CHECK(req(port, "GET", "/nope", &b4, &h4) == 404);
        CHECK(b4 == "{\"error\":\"not_found\"}");                 // 框架默认中性错误码（业务可经 render_error 覆盖）
        CHECK(h4.find("X-After: 1") != std::string::npos);
        CHECK(req(port, "GET", "/devices/%zz", &b0, &h0) == 400); // 非法百分号编码
        CHECK(h0.find("X-After: 1") != std::string::npos);
    }
    // 显式 HEAD/OPTIONS 优先于自动合成
    {
        std::string b, h;
        CHECK(req(port, "OPTIONS", "/explicit", &b, &h) == 200);  // 显式 OPTIONS handler（非 204）
        CHECK(b == "EXPLICIT-OPT");
        std::string h2;
        int st = req(port, "HEAD", "/hexp", 0, &h2, /*head_only=*/true);
        CHECK(st == 200 && h2.find("X-Head: explicit") != std::string::npos);  // 显式 HEAD handler
    }
    // Keep-Alive：HEAD 后紧接 GET，suppress_body 不得串请求（GET 返回完整实体）
    {
        int fd = dial(port);
        CHECK(fd >= 0);
        std::string buf;
        // HEAD（无实体，读到 header 结束即止）
        send_all(fd, "HEAD /devices/123 HTTP/1.1\r\nHost: t\r\nConnection: keep-alive\r\n\r\n");
        size_t he1 = std::string::npos;
        while ((he1 = buf.find("\r\n\r\n")) == std::string::npos) { char t[4096]; ssize_t n = ::recv(fd, t, sizeof(t), 0); if (n <= 0) break; buf.append(t, (size_t)n); }
        CHECK(he1 != std::string::npos);
        CHECK(status_of(buf) == 200);
        CHECK(buf.substr(0, he1).find("Content-Length: 6") != std::string::npos);
        buf.erase(0, he1 + 4);                                    // 消费 HEAD 响应（无 body）
        // GET（应返回完整实体）
        send_all(fd, "GET /devices/123 HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        size_t he2 = std::string::npos;
        while ((he2 = buf.find("\r\n\r\n")) == std::string::npos) { char t[4096]; ssize_t n = ::recv(fd, t, sizeof(t), 0); if (n <= 0) break; buf.append(t, (size_t)n); }
        CHECK(he2 != std::string::npos);
        size_t cl = 0, p = buf.find("Content-Length:"); if (p != std::string::npos) cl = (size_t)std::atol(buf.c_str() + p + 15);
        size_t bs = he2 + 4;
        while (buf.size() - bs < cl) { char t[4096]; ssize_t n = ::recv(fd, t, sizeof(t), 0); if (n <= 0) break; buf.append(t, (size_t)n); }
        ::close(fd);
        CHECK(buf.substr(bs, cl) == "id=123");                   // suppress 未串到下一请求
    }

    srv.stop();

    if (g_fail == 0) { printf("test_http_routing: ALL PASS\n"); return 0; }
    printf("test_http_routing: %d FAIL\n", g_fail);
    return 1;
}
