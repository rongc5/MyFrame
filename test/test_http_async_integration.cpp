// test_http_async_integration.cpp · 真实 HTTP 服务上的两阶段异步生命周期集成测试
//   启动 Level 2 myframe 服务，用原始 socket 客户端覆盖：
//     1) 两阶段异步成功       → 200 + 正确 body
//     2) work 抛异常          → 500
//     3) on_success 抛异常     → 500，且服务不崩（后续请求仍 200）
//     4) 执行器队列拒绝        → 503（小池 + 并发突发）
//     5) 客户端提前断开        → 不崩、不执行失效回调（后续请求仍 200）
//     8) Keep-Alive 下 POST body → GET，第二个请求 body 为空（跨请求不泄漏）
//   （6 代号丢弃 / 7 finalize 幂等 / 9 shutdown 由 test_http_context、test_task_executor 单测覆盖）
// 对应：fix(http): enforce async finalize-once and reset full request state（1c）
#include "server.h"
#include "unified_protocol_factory.h"
#include "protocol_context.h"
#include "http_middleware.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// 断连测试用：/slow-cnt 的 work / on_success 各自计数（证明失效连接回调不执行）
static std::atomic<int> g_slowcnt_work(0);
static std::atomic<int> g_slowcnt_success(0);
// 中间件 after 在异步路径的执行计数（证明异步也跑一次 after、且在发送前加头）
static std::atomic<int> g_after_count(0);

// ---------------- 被测服务处理器（Level 2，使用两阶段 async_response）----------------
class AsyncItHandler : public myframe::IProtocolHandler {
public:
    AsyncItHandler() {
        // after：请求结束钩子——异步完成时（finalize_once 内）逆序执行，在字节发送前加头
        _chain.use(myframe::Middleware(myframe::BeforeMiddleware(),
            [](myframe::HttpContext& c, myframe::CompletionReason) {
                g_after_count.fetch_add(1);
                c.response().set_header("X-After", "1");
            }));
    }

    void on_http_request(myframe::HttpContext& ctx) override {
        const std::string url = ctx.request().url;
        if (url == "/mw-async") {
            // 经中间件链执行一个异步 handler：after 应交由 finalize_once 执行
            myframe::RouteHandler h = [](myframe::HttpContext& c) {
                std::function<std::string()> work = []() { return std::string("mw"); };
                std::function<void(myframe::HttpContext&, std::string)> ok =
                    [](myframe::HttpContext& cc, std::string v) { cc.response().set_text(v); };
                c.async_response(work, ok);
            };
            _chain.execute(ctx, h);
            return;
        }
        if (url == "/mw-sync") {
            // 同步 handler 经中间件链；Keep-Alive 下连续两次应各跑一次 after
            ctx.keep_alive(true);
            myframe::RouteHandler h = [](myframe::HttpContext& c) { c.response().set_text("s"); };
            _chain.execute(ctx, h);
            return;
        }
        if (url == "/ok") {
            // 记录 handler（网络线程）线程 id；on_success 比对以证明回到了同一网络线程
            std::shared_ptr<std::thread::id> htid =
                std::make_shared<std::thread::id>(std::this_thread::get_id());
            std::function<std::string()> work = []() { return std::string("hi"); };
            std::function<void(myframe::HttpContext&, std::string)> ok =
                [htid](myframe::HttpContext& c, std::string v) {
                    // 相同线程 → 回显 v；否则显式暴露 WRONGTHREAD（客户端断言 body=="hi" 即证明同网络线程）
                    c.response().set_text(std::this_thread::get_id() == *htid ? v : "WRONGTHREAD");
                };
            ctx.async_response(work, ok);
        } else if (url == "/slow-cnt") {          // 断连测试专用：work/on_success 各计数
            std::function<std::string()> work = []() {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                g_slowcnt_work.fetch_add(1);      // work 确实执行
                return std::string("slow");
            };
            std::function<void(myframe::HttpContext&, std::string)> ok =
                [](myframe::HttpContext& c, std::string v) {
                    g_slowcnt_success.fetch_add(1); // on_success 若被错误执行会+1
                    c.response().set_text(v);
                };
            ctx.async_response(work, ok);
        } else if (url == "/work-throw") {
            std::function<std::string()> work = []() -> std::string { throw std::runtime_error("work-boom"); };
            std::function<void(myframe::HttpContext&, std::string)> ok =
                [](myframe::HttpContext& c, std::string v) { c.response().set_text(v); };
            ctx.async_response(work, ok);            // work 抛 → 框架 500
        } else if (url == "/done-throw") {
            std::function<std::string()> work = []() { return std::string("x"); };
            std::function<void(myframe::HttpContext&, std::string)> ok =
                [](myframe::HttpContext&, std::string) { throw std::runtime_error("done-boom"); };
            ctx.async_response(work, ok);            // on_success 抛 → 框架 500
        } else if (url == "/slow") {
            std::function<std::string()> work = []() {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                return std::string("slow");
            };
            std::function<void(myframe::HttpContext&, std::string)> ok =
                [](myframe::HttpContext& c, std::string v) { c.response().set_text(v); };
            ctx.async_response(work, ok);
        } else if (url == "/echolen") {              // 同步；Keep-Alive；回显请求体长度
            ctx.keep_alive(true);
            ctx.response().set_text(std::to_string(ctx.request().body.size()));
        } else {
            ctx.response().status = 404;
            ctx.response().set_text("nf");
        }
    }
    bool handle_thread_msg(std::shared_ptr<::normal_msg>&) override { return true; }
    void handle_timeout(std::shared_ptr<timer_msg>&) override {}

private:
    myframe::MiddlewareChain _chain;
};

// ---------------- 极简阻塞 HTTP 客户端 ----------------
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
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) break;
        off += (size_t)n;
    }
}
static int parse_status(const std::string& resp) {
    // "HTTP/1.1 200 ..." → 200
    size_t sp = resp.find(' ');
    if (sp == std::string::npos) return -1;
    return std::atoi(resp.c_str() + sp + 1);
}
// 读一条完整响应（依据 Content-Length 定帧）；返回 headers+body 原文，out_body 为响应体
static std::string read_one(int fd, std::string& out_body, std::string& carry) {
    std::string buf = carry; carry.clear();
    size_t hdr_end;
    while ((hdr_end = buf.find("\r\n\r\n")) == std::string::npos) {
        char tmp[4096]; ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return std::string();            // 断开/超时
        buf.append(tmp, (size_t)n);
    }
    // Content-Length
    size_t cl = 0; std::string head = buf.substr(0, hdr_end);
    size_t p = head.find("Content-Length:");
    if (p == std::string::npos) p = head.find("content-length:");
    if (p != std::string::npos) cl = (size_t)std::atol(head.c_str() + p + 15);
    size_t body_start = hdr_end + 4;
    while (buf.size() - body_start < cl) {
        char tmp[4096]; ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
    }
    out_body = buf.substr(body_start, cl);
    carry = buf.substr(body_start + cl);             // 多读的留给下一条（Keep-Alive）
    return buf.substr(0, body_start + cl);
}
// 单次请求（Connection: close）
static int req_once(unsigned short port, const std::string& method,
                    const std::string& path, const std::string& body, std::string* out_body = nullptr) {
    int fd = dial(port);
    if (fd < 0) return -1;
    std::string r = method + " " + path + " HTTP/1.1\r\nHost: t\r\nConnection: close\r\n";
    if (!body.empty()) r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "\r\n" + body;
    send_all(fd, r);
    std::string b, carry;
    std::string resp = read_one(fd, b, carry);
    ::close(fd);
    if (out_body) *out_body = b;
    return resp.empty() ? -1 : parse_status(resp);
}

int main(int argc, char** argv) {
    // 小执行器池，便于触发队列拒绝（默认 4/1024 无法在测试内打满）
    ::setenv("MYFRAME_ASYNC_THREADS", "1", 1);
    ::setenv("MYFRAME_ASYNC_QUEUE", "2", 1);

    unsigned short port = (argc > 1) ? (unsigned short)std::atoi(argv[1]) : 18099;

    auto handler = std::make_shared<AsyncItHandler>();
    auto factory = std::make_shared<myframe::UnifiedProtocolFactory>();
    factory->register_http_context_handler(handler.get());

    ::server srv(2);
    srv.bind("127.0.0.1", port);
    srv.set_business_factory(factory);
    srv.start();

    // 等端口就绪
    for (int i = 0; i < 100; ++i) {
        int fd = dial(port);
        if (fd >= 0) { ::close(fd); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // 1) 两阶段异步成功
    {
        std::string b; int st = req_once(port, "GET", "/ok", "", &b);
        CHECK(st == 200); CHECK(b == "hi");
    }
    // 1b) 中间件 after 在异步路径：跑一次、发送前加头
    {
        g_after_count.store(0);
        int fd = dial(port);
        CHECK(fd >= 0);
        send_all(fd, "GET /mw-async HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        std::string b, carry;
        std::string resp = read_one(fd, b, carry);
        ::close(fd);
        CHECK(parse_status(resp) == 200);
        CHECK(b == "mw");
        CHECK(resp.find("X-After: 1") != std::string::npos);   // after 在发送前加了头
        CHECK(g_after_count.load() == 1);                      // 异步也只跑一次 after
    }
    // 2) work 抛异常 → 500
    {
        int st = req_once(port, "GET", "/work-throw", "");
        CHECK(st == 500);
    }
    // 3) on_success 抛异常 → 500，且服务不崩（随后 /ok 仍 200）
    {
        int st = req_once(port, "GET", "/done-throw", "");
        CHECK(st == 500);
        std::string b; int st2 = req_once(port, "GET", "/ok", "", &b);
        CHECK(st2 == 200 && b == "hi");              // 服务存活
    }
    // 8) Keep-Alive：POST body=secret → GET，第二个请求 body 长度为 0
    {
        int fd = dial(port);
        CHECK(fd >= 0);
        std::string carry, b1, b2;
        send_all(fd, "POST /echolen HTTP/1.1\r\nHost: t\r\nConnection: keep-alive\r\n"
                     "Content-Length: 6\r\n\r\nsecret");
        std::string r1 = read_one(fd, b1, carry);
        CHECK(parse_status(r1) == 200); CHECK(b1 == "6");        // 第一个请求看到 6 字节
        send_all(fd, "GET /echolen HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        std::string r2 = read_one(fd, b2, carry);
        CHECK(parse_status(r2) == 200); CHECK(b2 == "0");        // 第二个请求 body 为空（无泄漏）
        ::close(fd);
    }
    // 7) Keep-Alive 连续请求：每次各跑一次 after（after 栈不跨请求累积）
    {
        g_after_count.store(0);
        int fd = dial(port);
        CHECK(fd >= 0);
        std::string carry, b1, b2;
        send_all(fd, "GET /mw-sync HTTP/1.1\r\nHost: t\r\nConnection: keep-alive\r\n\r\n");
        std::string r1 = read_one(fd, b1, carry);
        CHECK(parse_status(r1) == 200 && b1 == "s");
        CHECK(r1.find("X-After: 1") != std::string::npos);
        send_all(fd, "GET /mw-sync HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        std::string r2 = read_one(fd, b2, carry);
        CHECK(parse_status(r2) == 200 && b2 == "s");
        CHECK(r2.find("X-After: 1") != std::string::npos);
        ::close(fd);
        CHECK(g_after_count.load() == 2);              // 两请求各一次；若跨请求累积会是 3
    }
    // 5) 客户端在 work 完成前断开 → 不崩、work 仍执行、但失效回调 on_success 不执行
    {
        g_slowcnt_work.store(0);
        g_slowcnt_success.store(0);
        int fd = dial(port);
        CHECK(fd >= 0);
        send_all(fd, "GET /slow-cnt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        ::close(fd);                                  // 立刻断开（work 需 300ms）
        std::this_thread::sleep_for(std::chrono::milliseconds(600));  // 等 work 完成并尝试回投
        CHECK(g_slowcnt_work.load() == 1);            // work 确实跑了（不是被跳过）
        CHECK(g_slowcnt_success.load() == 0);         // 连接已失效 → on_success 被安全丢弃、未执行
        std::string b; int st = req_once(port, "GET", "/ok", "", &b);
        CHECK(st == 200 && b == "hi");                // 服务存活
    }
    // 4) 队列拒绝 → 503（并发突发 6 个 /slow，容量=线程1+队列2=3）
    {
        const int N = 6;
        std::vector<int> fds(N, -1);
        for (int i = 0; i < N; ++i) {
            fds[i] = dial(port);
            if (fds[i] >= 0)
                send_all(fds[i], "GET /slow HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        }
        int ok = 0, rej = 0, other = 0;
        for (int i = 0; i < N; ++i) {
            if (fds[i] < 0) { ++other; continue; }
            std::string b, carry;
            std::string resp = read_one(fds[i], b, carry);
            int st = resp.empty() ? -1 : parse_status(resp);
            if (st == 200) ++ok; else if (st == 503) ++rej; else ++other;
            ::close(fds[i]);
        }
        printf("  reject burst: 200=%d 503=%d other=%d\n", ok, rej, other);
        CHECK(rej >= 1);                              // 至少一个被拒 → 503
        CHECK(ok >= 1);                               // 至少一个成功
    }

    srv.stop();

    if (g_fail == 0) { printf("test_http_async_integration: ALL PASS\n"); return 0; }
    printf("test_http_async_integration: %d FAIL\n", g_fail);
    return 1;
}
