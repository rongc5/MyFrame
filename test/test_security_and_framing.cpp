// Batch 1 security, correctness and framing regression suite (C++11).
//
// Every scenario is deterministic and self-contained:
//   1. HTTP smuggling and strict framing (exact header lookup, duplicate and
//      malformed Content-Length / Transfer-Encoding, obs-fold, NUL bytes,
//      header count, configurable body cap).                  [N1, C1, N1b]
//   2. Chunked size overflow and strict hex parsing.           [N6]
//   3. Keep-alive status line and custom reason phrases.       [N7]
//   4. Protocol detection: byte-at-a-time, split headers and pipelined
//      requests through ProtocolDetector and the legacy detector.  [N8]
//   5. Binary TLV adapters: oversized length and split packets.    [N2, N3b, N20]
//   6. WebSocket: unmasked client frames, message cap, O(1) header parsing,
//      PING/PONG handling.                                     [N11, C3, W-c]
//   7. TLS exact protocol range ("TLSv1.3").                   [T-a]
//   8. HTTP/2 split frames, client :status mapping and event-loop mutation
//      during a tick.                                          [N3, N21, N10]
#include "base_def.h"
#include "common_def.h"
#include "common_exception.h"
#include "common_obj_container.h"
#include "base_net_obj.h"
#include "http_base_data_process.h"
#include "http_res_process.h"
#include "http_req_process.h"
#include "protocol_limits.h"
#include "web_socket_msg.h"
#include "http2_frame.h"
#include "http2_process.h"
#include "http2_client_process.h"
#include "app_handler_v2.h"
#include "protocol_context.h"
#include "protocol_adapters/binary_application_adapter.h"
#include "protocol_adapters/binary_context_adapter.h"
#include "server.h"
#include "ssl_context.h"
#include "unified_protocol_factory.h"
#include "multi_protocol_factory.h"
#include "ws_test_client.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

static std::atomic<int> g_fail(0);
static std::atomic<int> g_checks(0);

#define CHECK(c)                                                              \
    do {                                                                      \
        g_checks.fetch_add(1);                                                \
        if (!(c)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                  \
            std::fflush(stdout);                                              \
            g_fail.fetch_add(1);                                              \
        }                                                                     \
    } while (0)

static void section(const char* name) {
    std::printf("== %s\n", name);
    std::fflush(stdout);
}

static uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ===========================================================================
// HTTP/1.x parser harness (no sockets)
// ===========================================================================

class BodyConsumer : public http_base_data_process {
public:
    explicit BodyConsumer(http_base_process* owner)
        : http_base_data_process(owner), finishes(0) {}

    size_t process_recv_body(const char* buf, size_t len, int& result) override {
        if (len > 0) body.append(buf, len);
        result = 1;
        return len;
    }

    void msg_recv_finish() override { ++finishes; }

    std::string body;
    int finishes;
};

class ProbeServer : public http_res_process {
public:
    ProbeServer() : http_res_process(std::shared_ptr<base_net_obj>()) {
        consumer = new BodyConsumer(this);
        set_process(consumer);
    }
    using http_res_process::parse_header;

    BodyConsumer* consumer;
};

class ProbeClient : public http_req_process {
public:
    ProbeClient() : http_req_process(std::shared_ptr<base_net_obj>()) {
        consumer = new BodyConsumer(this);
        set_process(consumer);
        change_http_status(RECV_HEAD, false);
    }

    BodyConsumer* consumer;
};

// Mimics base_connect::real_recv: unconsumed bytes stay pending and are
// presented again (together with new bytes) on the next call.
template <typename Proc>
static size_t feed_like_connection(Proc& proc, std::string& pending, const std::string& chunk) {
    pending.append(chunk);
    size_t r = proc.process_recv_buf(pending.data(), pending.size());
    if (r > pending.size()) r = pending.size();
    pending.erase(0, r);
    return r;
}

// True when feeding wire to a fresh server parser raises an exception.
static bool server_rejects(const std::string& wire, std::string* body_seen = NULL) {
    ProbeServer p;
    bool threw = false;
    try {
        p.process_recv_buf(wire.data(), wire.size());
    } catch (const std::exception&) {
        threw = true;
    }
    if (body_seen) *body_seen = p.consumer->body;
    return threw;
}

static bool header_block_rejected(const std::string& block) {
    ProbeServer p;
    std::string b = block;
    try {
        p.parse_header(b);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

static std::string post_with(const std::string& extra_headers, const std::string& body) {
    return "POST /x HTTP/1.1\r\nHost: t\r\n" + extra_headers + "\r\n" + body;
}

static void test_http_smuggling_and_framing() {
    section("1. HTTP smuggling and strict framing [N1, C1, N1b]");

    // Exact lookup: A-Content-Length must not shadow Content-Length.
    {
        ProbeServer p;
        const std::string first =
            "POST /a HTTP/1.1\r\nHost: t\r\nA-Content-Length: 50\r\nContent-Length: 0\r\n\r\n";
        const std::string second = "GET /b HTTP/1.1\r\nHost: t\r\n\r\n";
        const std::string wire = first + second;
        size_t consumed = p.process_recv_buf(wire.data(), wire.size());
        CHECK(consumed == first.size());
        CHECK(p.consumer->finishes == 1);
        CHECK(p.consumer->body.empty());
        std::string* cl = p.get_req_head_para().get_header("Content-Length");
        CHECK(cl != NULL && *cl == "0");
        CHECK(p.get_req_head_para().get_header("content-length") != NULL);
        CHECK(p.get_req_head_para().get_header("Length") == NULL);
        p.reset();
        size_t c2 = p.process_recv_buf(wire.data() + consumed, wire.size() - consumed);
        CHECK(c2 == second.size());
        CHECK(p.consumer->finishes == 2);
        CHECK(p.get_req_head_para()._url_path == "/b");
    }
    // Exact lookup on the response side as well.
    {
        http_res_head_para rh;
        rh._headers["X-Connection-Info"] = "close";
        CHECK(rh.get_header("Connection") == NULL);
        rh._headers["connection"] = "keep-alive";
        CHECK(rh.get_header("Connection") != NULL && *rh.get_header("Connection") == "keep-alive");
    }
    // X-Transfer-Encoding is an ordinary header and never enables chunked.
    {
        ProbeServer p;
        const std::string wire = post_with("X-Transfer-Encoding: chunked\r\nContent-Length: 5\r\n", "hello");
        bool threw = false;
        size_t consumed = 0;
        try {
            consumed = p.process_recv_buf(wire.data(), wire.size());
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(!threw);
        CHECK(consumed == wire.size());
        CHECK(p.consumer->body == "hello");
        CHECK(p.consumer->finishes == 1);
    }
    // Duplicate / conflicting framing headers (including mixed case).
    CHECK(server_rejects(post_with("Content-Length: 5\r\nContent-Length: 5\r\n", "hello")));
    CHECK(server_rejects(post_with("Content-Length: 5\r\ncontent-length: 6\r\n", "hello")));
    CHECK(server_rejects(post_with("Content-Length: 5\r\ncontent-length: 5\r\n", "hello")));
    CHECK(server_rejects(post_with("CONTENT-LENGTH: 0\r\ncontent-Length: 5\r\n", "hello")));
    CHECK(server_rejects(post_with("Transfer-Encoding: chunked\r\ntransfer-encoding: chunked\r\n", "0\r\n\r\n")));
    CHECK(server_rejects(post_with("Transfer-Encoding: chunked\r\nContent-Length: 5\r\n", "0\r\n\r\n")));
    CHECK(server_rejects(post_with("Transfer-Encoding: gzip, chunked\r\n", "0\r\n\r\n")));
    CHECK(server_rejects(post_with("Transfer-Encoding: identity\r\n", "")));
    CHECK(server_rejects("GET / HTTP/1.1\r\nHost: a\r\nhost: b\r\n\r\n"));
    // Malformed Content-Length values.
    CHECK(server_rejects(post_with("Content-Length: -1\r\n", "")));
    CHECK(server_rejects(post_with("Content-Length: +5\r\n", "hello")));
    CHECK(server_rejects(post_with("Content-Length: 5abc\r\n", "hello")));
    CHECK(server_rejects(post_with("Content-Length: 0x5\r\n", "hello")));
    CHECK(server_rejects(post_with("Content-Length: 5 5\r\n", "hello")));
    CHECK(server_rejects(post_with("Content-Length: 5,5\r\n", "hello")));
    CHECK(server_rejects(post_with("Content-Length:\r\n", "")));
    CHECK(server_rejects(post_with("Content-Length: 18446744073709551616\r\n", "")));
    // Whitespace between field name and colon (RFC 9112 5.1).
    CHECK(server_rejects(post_with("Content-Length : 5\r\n", "hello")));
    CHECK(server_rejects(post_with("Content-Length\t: 5\r\n", "hello")));
    // Obsolete line folding.
    CHECK(server_rejects(post_with("X-A: 1\r\n folded\r\nContent-Length: 0\r\n", "")));
    CHECK(server_rejects(post_with("X-A: 1\r\n\tfolded\r\nContent-Length: 0\r\n", "")));
    // NUL byte inside the header block.
    {
        std::string wire = "POST /x HTTP/1.1\r\nHost: t\r\nX-A: a";
        wire.push_back('\0');
        wire += "b\r\nContent-Length: 0\r\n\r\n";
        CHECK(server_rejects(wire));
    }
    // Missing colon, empty name, bad token characters, bare CR.
    CHECK(server_rejects(post_with("NoColonHere\r\n", "")));
    CHECK(server_rejects(post_with(": value\r\n", "")));
    CHECK(server_rejects(post_with("Bad(Name): v\r\n", "")));
    CHECK(server_rejects(post_with("X-A: a\rb\r\n", "")));
    // Optional whitespace around the value is still accepted.
    {
        ProbeServer p;
        const std::string wire = post_with("Content-Length: \t 5 \t\r\n", "hello");
        bool threw = false;
        try {
            p.process_recv_buf(wire.data(), wire.size());
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(!threw);
        CHECK(p.consumer->body == "hello");
    }
    // Header count limit.
    {
        std::string ok_block = "GET / HTTP/1.1\r\n";
        for (size_t i = 0; i < myframe::MAX_HTTP_HEADER_COUNT; ++i) {
            ok_block += "X-H" + std::to_string(static_cast<unsigned long long>(i)) + ": v\r\n";
        }
        CHECK(!header_block_rejected(ok_block + "\r\n"));
        CHECK(header_block_rejected(ok_block + "X-One-Too-Many: v\r\n\r\n"));
    }
    // Configurable body cap: rejected at header time, before buffering.
    {
        myframe::set_max_http_body_bytes(4096);
        CHECK(myframe::max_http_body_bytes() == 4096);
        std::string big(8192, 'z');
        std::string seen;
        CHECK(server_rejects(post_with("Content-Length: 4097\r\n", big), &seen));
        CHECK(seen.empty());
        CHECK(server_rejects(post_with("Content-Length: 99999999999\r\n", big), &seen));
        CHECK(seen.empty());
        // Exactly at the cap is accepted.
        {
            ProbeServer p;
            const std::string wire = post_with("Content-Length: 4096\r\n", std::string(4096, 'y'));
            bool threw = false;
            try {
                p.process_recv_buf(wire.data(), wire.size());
            } catch (const std::exception&) {
                threw = true;
            }
            CHECK(!threw);
            CHECK(p.consumer->body.size() == 4096);
        }
        // Chunked bodies honour the same cap; nothing beyond it is buffered.
        {
            std::string chunked = "BB8\r\n" + std::string(3000, 'a') + "\r\n" +
                                  "BB8\r\n" + std::string(3000, 'b') + "\r\n0\r\n\r\n";
            CHECK(server_rejects(post_with("Transfer-Encoding: chunked\r\n", chunked), &seen));
            CHECK(seen.size() <= 4096);
        }
        myframe::set_max_http_body_bytes(0);
        CHECK(myframe::max_http_body_bytes() == myframe::kDefaultMaxHttpBodyBytes);
        CHECK(myframe::kDefaultMaxHttpBodyBytes == 16u * 1024u * 1024u);
    }
    // Strict decimal helper.
    {
        uint64_t v = 0;
        CHECK(myframe::parse_strict_decimal_u64("0", v) && v == 0);
        CHECK(myframe::parse_strict_decimal_u64("18446744073709551615", v) && v == 18446744073709551615ull);
        CHECK(!myframe::parse_strict_decimal_u64("18446744073709551616", v));
        CHECK(!myframe::parse_strict_decimal_u64("", v));
        CHECK(!myframe::parse_strict_decimal_u64(" 1", v));
        CHECK(!myframe::parse_strict_decimal_u64("1 ", v));
    }
    // Client side: negative / garbage / conflicting Content-Length rejected.
    {
        const char* bad[] = {
            "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 5abc\r\n\r\nhello",
            "HTTP/1.1 200 OK\r\nContent-Length: +5\r\n\r\nhello",
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\ncontent-length: 7\r\n\r\nhello",
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            ProbeClient c;
            std::string wire(bad[i]);
            bool threw = false;
            try {
                c.process_recv_buf(wire.data(), wire.size());
            } catch (const std::exception&) {
                threw = true;
            }
            CHECK(threw);
        }
        ProbeClient ok;
        std::string wire("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
        bool threw = false;
        try {
            ok.process_recv_buf(wire.data(), wire.size());
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(!threw);
        CHECK(ok.consumer->body == "hello");
        CHECK(ok.consumer->finishes == 1);
    }
}

// ===========================================================================
// 2. Chunked size overflow [N6]
// ===========================================================================

static void test_chunked_overflow() {
    section("2. Chunked size overflow [N6]");
    const std::string head = "POST /c HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n";
    std::string seen;
    CHECK(server_rejects(head + "FFFFFFFFFFFFFFF8\r\n" + std::string(64, 'x'), &seen));
    CHECK(seen.empty());
    CHECK(server_rejects(head + "FFFFFFFFFFFFFFFF\r\n", &seen));
    CHECK(server_rejects(head + "00000000000000010\r\nabcdefghijklmnop\r\n0\r\n\r\n", &seen));
    CHECK(seen.empty());
    CHECK(server_rejects(head + "0x10\r\nabcdefghijklmnop\r\n0\r\n\r\n", &seen));
    CHECK(seen.empty());
    CHECK(server_rejects(head + "+5\r\nhello\r\n0\r\n\r\n"));
    CHECK(server_rejects(head + "-5\r\nhello\r\n0\r\n\r\n"));
    CHECK(server_rejects(head + " 5\r\nhello\r\n0\r\n\r\n"));
    CHECK(server_rejects(head + "5\r\nhelloXX0\r\n\r\n"));
    // A single chunk above the chunked body cap (MAX_CHUNKED_BODY_LEN).
    CHECK(server_rejects(head + "800001\r\n"));
    {
        uint64_t v = 0;
        CHECK(myframe::parse_strict_hex_u64("ffffffffffffffff", v) && v == 0xffffffffffffffffull);
        CHECK(!myframe::parse_strict_hex_u64("1ffffffffffffffff", v));
        CHECK(!myframe::parse_strict_hex_u64("0x1", v));
        CHECK(!myframe::parse_strict_hex_u64("", v));
    }
    // A valid chunked request (with extension and trailer) still works, and
    // a pipelined request after it is left untouched.
    {
        ProbeServer p;
        const std::string body = "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nX-Trailer: t\r\n\r\n";
        const std::string next = "GET /n HTTP/1.1\r\nHost: t\r\n\r\n";
        const std::string wire = head + body + next;
        size_t consumed = p.process_recv_buf(wire.data(), wire.size());
        CHECK(consumed == head.size() + body.size());
        CHECK(p.consumer->body == "hello world");
        CHECK(p.consumer->finishes == 1);
    }
    // The same request fed one byte at a time.
    {
        ProbeServer p;
        const std::string wire = head + "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        std::string pending;
        for (size_t i = 0; i < wire.size(); ++i) {
            feed_like_connection(p, pending, wire.substr(i, 1));
        }
        CHECK(pending.empty());
        CHECK(p.consumer->body == "hello world");
        CHECK(p.consumer->finishes == 1);
    }
}

// ===========================================================================
// Socket helpers
// ===========================================================================

static int tcp_connect(unsigned short port, int timeout_ms = 3000) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static bool send_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

struct HttpReply {
    std::string status_line;
    std::string headers;
    std::string body;
};

static std::string lower_copy(const std::string& s) {
    std::string r = s;
    for (size_t i = 0; i < r.size(); ++i) r[i] = static_cast<char>(tolower(static_cast<unsigned char>(r[i])));
    return r;
}

// Read one Content-Length framed response; bytes after it stay in buf.
static bool read_http_reply(int fd, std::string& buf, HttpReply& out) {
    char tmp[16384];
    size_t hend = std::string::npos;
    for (;;) {
        hend = buf.find("\r\n\r\n");
        if (hend != std::string::npos) break;
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
    }
    std::string head = buf.substr(0, hend);
    size_t eol = head.find("\r\n");
    out.status_line = head.substr(0, eol);
    out.headers = (eol == std::string::npos) ? std::string() : head.substr(eol + 2);
    size_t clen = 0;
    std::string lh = lower_copy(out.headers);
    size_t p = lh.find("content-length:");
    if (p != std::string::npos) {
        clen = static_cast<size_t>(std::strtoull(lh.c_str() + p + 15, NULL, 10));
    }
    buf.erase(0, hend + 4);
    while (buf.size() < clen) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
    }
    out.body = buf.substr(0, clen);
    buf.erase(0, clen);
    return true;
}

// ===========================================================================
// Server-side handlers
// ===========================================================================

class CtxHandler : public myframe::IProtocolHandler {
public:
    void on_http_request(myframe::HttpContext& ctx) override {
        const myframe::HttpRequest& req = ctx.request();
        myframe::HttpResponse& res = ctx.response();
        res.set_content_type("text/plain");
        if (req.url.compare(0, 7, "/custom") == 0) {
            res.status = 201;
            res.reason = "Custom Created";
            res.body = "custom";
        } else if (req.url.compare(0, 3, "/nf") == 0) {
            res.status = 404;
            res.body = "nf";
        } else if (req.url.compare(0, 5, "/echo") == 0) {
            res.body = req.body;
        } else {
            res.body = "ok:" + req.url;
        }
    }
};

static std::atomic<long> g_ws_counted(0);

class WsApp : public myframe::IApplicationHandler {
public:
    void on_ws(const myframe::WsFrame& recv, myframe::WsFrame& send) override {
        if (recv.payload == "END") {
            send = myframe::WsFrame::text(std::to_string(static_cast<long long>(g_ws_counted.load())));
        } else if (recv.payload.compare(0, 5, "echo:") == 0) {
            send = myframe::WsFrame::text(recv.payload);
        } else {
            g_ws_counted.fetch_add(1);
            send = myframe::WsFrame::text("");
        }
    }
};

class LegacyApp : public myframe::IApplicationHandler {
public:
    void on_http(const myframe::HttpRequest& req, myframe::HttpResponse& res) override {
        res.body = "legacy:" + req.url;
        res.headers["Connection"] = "keep-alive";
    }
};

// ===========================================================================
// 3. Keep-alive status line [N7]
// ===========================================================================

static void test_status_line_unit() {
    section("3a. Status line defaults and custom reasons (unit) [N7]");
    http_res_head_para p;
    p.init();
    CHECK(p._version == "HTTP/1.1");
    CHECK(p._response_str == "OK");
    std::string h;
    p.to_head_str(&h);
    CHECK(h.compare(0, 17, "HTTP/1.1 200 OK\r\n") == 0);

    p.init();
    p._response_code = 201;
    p._response_str = "Custom Created";
    h.clear();
    p.to_head_str(&h);
    CHECK(h.compare(0, 29, "HTTP/1.1 201 Custom Created\r\n") == 0);

    p.init();
    p._response_code = 404;
    h.clear();
    p.to_head_str(&h);
    CHECK(h.compare(0, 24, "HTTP/1.1 404 Not Found\r\n") == 0);

    p.init();
    p._version.clear();
    p._response_str.clear();
    h.clear();
    p.to_head_str(&h);
    CHECK(h.compare(0, 17, "HTTP/1.1 200 OK\r\n") == 0);
}

static void test_keepalive_status_line(unsigned short port) {
    section("3b. Keep-alive status line over one HttpContextAdapter connection [N7]");
    int fd = tcp_connect(port);
    CHECK(fd >= 0);
    if (fd < 0) return;
    const char* paths[] = {"/ok", "/custom", "/ok", "/nf", "/custom", "/ok"};
    const char* expect[] = {"HTTP/1.1 200 OK", "HTTP/1.1 201 Custom Created", "HTTP/1.1 200 OK",
                            "HTTP/1.1 404 Not Found", "HTTP/1.1 201 Custom Created", "HTTP/1.1 200 OK"};
    std::string buf;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        std::string req = std::string("GET ") + paths[i] + " HTTP/1.1\r\nHost: t\r\n\r\n";
        CHECK(send_all(fd, req));
        HttpReply r;
        bool ok = read_http_reply(fd, buf, r);
        CHECK(ok);
        if (!ok) break;
        CHECK(r.status_line == expect[i]);
        CHECK(r.status_line.compare(0, 9, "HTTP/1.1 ") == 0);
    }
    ::close(fd);
}

// ===========================================================================
// 4. Protocol detection handoff [N8]
// ===========================================================================

static void test_detector_handoff(unsigned short port, unsigned short legacy_port) {
    section("4. Protocol detection byte-at-a-time and pipelined handoff [N8]");
    // (a) One byte per TCP segment through ProtocolDetector.
    {
        int fd = tcp_connect(port);
        CHECK(fd >= 0);
        if (fd >= 0) {
            const std::string req =
                "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 11\r\n\r\nhello world";
            bool sent = true;
            for (size_t i = 0; i < req.size() && sent; ++i) {
                sent = send_all(fd, req.substr(i, 1));
                sleep_ms(1);
            }
            CHECK(sent);
            std::string buf;
            HttpReply r;
            CHECK(read_http_reply(fd, buf, r));
            CHECK(r.status_line == "HTTP/1.1 200 OK");
            CHECK(r.body == "hello world");
            // The connection stays usable after the handoff.
            CHECK(send_all(fd, "GET /after HTTP/1.1\r\nHost: t\r\n\r\n"));
            HttpReply r2;
            CHECK(read_http_reply(fd, buf, r2));
            CHECK(r2.body == "ok:/after");
            ::close(fd);
        }
    }
    // (b) Two pipelined requests in a single burst through ProtocolDetector.
    {
        int fd = tcp_connect(port);
        CHECK(fd >= 0);
        if (fd >= 0) {
            const std::string body = "pipelined-body-0123456789";
            const std::string burst =
                "GET /first HTTP/1.1\r\nHost: t\r\n\r\n"
                "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: " +
                std::to_string(static_cast<unsigned long long>(body.size())) + "\r\n\r\n" + body;
            CHECK(send_all(fd, burst));
            std::string buf;
            HttpReply r1, r2;
            CHECK(read_http_reply(fd, buf, r1));
            CHECK(r1.body == "ok:/first");
            bool got2 = read_http_reply(fd, buf, r2);
            CHECK(got2);
            CHECK(r2.body == body);
            CHECK(buf.empty());
            ::close(fd);
        }
    }
    // (c) Header split across two reads: the first chunk must not be dropped.
    {
        int fd = tcp_connect(port);
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(send_all(fd, "GET /split HTTP/1.1\r\nHo"));
            sleep_ms(50);
            CHECK(send_all(fd, "st: t\r\n\r\n"));
            std::string buf;
            HttpReply r;
            CHECK(read_http_reply(fd, buf, r));
            CHECK(r.body == "ok:/split");
            ::close(fd);
        }
    }
    // (d) Legacy protocol_detect_process: the probe matches on "GET " before
    // the header is complete; the partial header must survive the handoff.
    {
        int fd = tcp_connect(legacy_port);
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(send_all(fd, "GET /legacy-split HTTP/1.1\r\nHo"));
            sleep_ms(50);
            CHECK(send_all(fd, "st: t\r\n\r\n"));
            std::string buf;
            HttpReply r;
            CHECK(read_http_reply(fd, buf, r));
            CHECK(r.body == "legacy:/legacy-split");
            ::close(fd);
        }
    }
    // (e) Legacy detector, pipelined burst.
    {
        int fd = tcp_connect(legacy_port);
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(send_all(fd, "GET /l1 HTTP/1.1\r\nHost: t\r\n\r\nGET /l2 HTTP/1.1\r\nHost: t\r\n\r\n"));
            std::string buf;
            HttpReply r1, r2;
            CHECK(read_http_reply(fd, buf, r1));
            CHECK(r1.body == "legacy:/l1");
            CHECK(read_http_reply(fd, buf, r2));
            CHECK(r2.body == "legacy:/l2");
            ::close(fd);
        }
    }
}

// ===========================================================================
// 5. Binary TLV adapters [N2, N3b, N20]
// ===========================================================================

class BinApp : public myframe::IApplicationHandler {
public:
    BinApp() : errors(0) {}
    void on_binary(const myframe::BinaryRequest& req, myframe::BinaryResponse& res) override {
        messages.push_back(std::string(req.payload.begin(), req.payload.end()));
        ids.push_back(req.protocol_id);
        res.data = req.payload;
    }
    void on_error(const std::string&) override { ++errors; }
    std::vector<std::string> messages;
    std::vector<uint32_t> ids;
    int errors;
};

class BinCtxHandler : public myframe::IProtocolHandler {
public:
    void on_binary_message(myframe::BinaryContext& ctx) override {
        messages.push_back(std::string(reinterpret_cast<const char*>(ctx.payload()), ctx.payload_size()));
    }
    std::vector<std::string> messages;
};

static std::string host_tlv(uint32_t id, uint32_t len_field, const std::string& payload) {
    std::string f;
    f.append(reinterpret_cast<const char*>(&id), 4);
    f.append(reinterpret_cast<const char*>(&len_field), 4);
    f += payload;
    return f;
}

static std::string net_tlv(uint32_t id, uint32_t len_field, const std::string& payload) {
    uint32_t nid = htonl(id), nlen = htonl(len_field);
    std::string f;
    f.append(reinterpret_cast<const char*>(&nid), 4);
    f.append(reinterpret_cast<const char*>(&nlen), 4);
    f += payload;
    return f;
}

template <typename Proc>
static bool feed_chunks_exact(Proc& p, const std::string& wire, size_t step) {
    std::string pending;
    bool all_len = true;
    for (size_t off = 0; off < wire.size(); off += step) {
        std::string chunk = wire.substr(off, step);
        size_t before = pending.size() + chunk.size();
        size_t r = feed_like_connection(p, pending, chunk);
        if (r != before) all_len = false;
    }
    return all_len && pending.empty();
}

static void test_binary_adapters() {
    section("5. Binary TLV adapters overflow and split packets [N2, N3b, N20]");
    // Oversized length 0xFFFFFFF8 (the old 8 + len wrapped to 0).
    {
        BinApp app;
        myframe::BinaryApplicationAdapter a(std::shared_ptr<base_net_obj>(), &app);
        std::string wire = host_tlv(1, 0xFFFFFFF8u, "");
        bool threw = false;
        try {
            a.process_recv_buf(wire.data(), wire.size());
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(app.messages.empty());
    }
    {
        BinApp app;
        myframe::BinaryApplicationAdapter a(std::shared_ptr<base_net_obj>(), &app);
        std::string wire = host_tlv(1, static_cast<uint32_t>(myframe::BinaryApplicationAdapter::kMaxBinaryFrameLen + 1), "x");
        bool threw = false;
        try {
            a.process_recv_buf(wire.data(), wire.size());
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }
    // Valid messages split into 1-byte and 3-byte chunks: decoded exactly once.
    for (size_t step = 1; step <= 3; step += 2) {
        BinApp app;
        myframe::BinaryApplicationAdapter a(std::shared_ptr<base_net_obj>(), &app);
        const std::string m1 = "abcdefghij";
        const std::string m2 = "second-message-payload";
        std::string wire = host_tlv(7, static_cast<uint32_t>(m1.size()), m1) +
                           host_tlv(9, static_cast<uint32_t>(m2.size()), m2);
        CHECK(feed_chunks_exact(a, wire, step));
        CHECK(app.messages.size() == 2);
        if (app.messages.size() == 2) {
            CHECK(app.messages[0] == m1);
            CHECK(app.messages[1] == m2);
            CHECK(app.ids[0] == 7 && app.ids[1] == 9);
        }
        int replies = 0;
        for (;;) {
            std::string* out = a.get_send_buf();
            if (!out) break;
            ++replies;
            delete out;
        }
        CHECK(replies == 2);
    }
    // Context adapter: oversized frame closes instead of buffering forever.
    {
        BinCtxHandler h;
        std::unique_ptr<base_data_process> a =
            myframe::BinaryContextAdapter::create(std::shared_ptr<base_net_obj>(), &h);
        std::string wire = net_tlv(3, 0xFFFFFFF0u, "");
        bool threw = false;
        try {
            a->process_recv_buf(wire.data(), wire.size());
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(h.messages.empty());
    }
    {
        BinCtxHandler h;
        std::unique_ptr<base_data_process> a =
            myframe::BinaryContextAdapter::create(std::shared_ptr<base_net_obj>(), &h);
        const std::string m = "context-payload";
        std::string wire = net_tlv(5, static_cast<uint32_t>(m.size()), m) + net_tlv(6, 3, "xyz");
        CHECK(feed_chunks_exact(*a, wire, 3));
        CHECK(h.messages.size() == 2);
        if (h.messages.size() == 2) {
            CHECK(h.messages[0] == m);
            CHECK(h.messages[1] == "xyz");
        }
    }
}

// ===========================================================================
// 6. WebSocket hardening [N11, C3, W-c]
// ===========================================================================

static void test_ws_header_parse_speed() {
    section("6a. WebSocket frame header parsing is O(1) per frame [N11]");
    const int kFrames = 20000;
    std::string wire;
    wire.reserve(static_cast<size_t>(kFrames) * 8);
    for (int i = 0; i < kFrames; ++i) {
        wire += wstest::Client::build_frame(0x2, "ab", true, true, 0x01020304u + static_cast<uint32_t>(i));
    }
    web_socket_frame_header h;
    char* p = &wire[0];
    uint32_t left = static_cast<uint32_t>(wire.size());
    int frames = 0;
    size_t max_header = 0;
    bool payload_ok = true;
    const uint64_t t0 = now_ms();
    while (left > 0) {
        if (h._wb_body_status == WB_FRAME_HEAD_STAUS) {
            h.process(p, left);
            if (h._s_header.size() > max_header) max_header = h._s_header.size();
        }
        if (h._wb_body_status == WB_FRAME_BODY_STAUS) {
            uint32_t rem = h.update(left);
            uint32_t chunk = left - rem;
            std::string plain = h.mask_code(p, chunk);
            if (plain != "ab") payload_ok = false;
            p += chunk;
            left = rem;
            if (h.if_finish()) ++frames;
        }
    }
    const uint64_t elapsed = now_ms() - t0;
    std::printf("   parsed %d frames in %llu ms (max header %zu bytes)\n", frames,
                static_cast<unsigned long long>(elapsed), max_header);
    CHECK(frames == kFrames);
    CHECK(payload_ok);
    CHECK(max_header <= 14);
    CHECK(elapsed < 1000);

    // Header bytes arriving one at a time (extended 16-bit length + mask).
    {
        std::string big(300, 'q');
        std::string f = wstest::Client::build_frame(0x1, big, true, true);
        web_socket_frame_header hh;
        size_t i = 0;
        while (hh._wb_body_status == WB_FRAME_HEAD_STAUS && i < f.size()) {
            char* bp = &f[i];
            uint32_t l = 1;
            hh.process(bp, l);
            CHECK(l == 0);
            ++i;
        }
        CHECK(i == 8);  // 2 + 2 (extended length) + 4 (mask key)
        CHECK(hh._payload_len == 300);
        CHECK(hh._mask_flag == 1);
    }
}

static bool ws_read_text(wstest::Client& c, std::string& payload, int& op) {
    for (int i = 0; i < 3; ++i) {
        int rc = c.read_frame(payload, op);
        if (rc == 1) return true;
        if (rc < 0) return false;
    }
    return false;
}

static void test_ws_hardening(unsigned short port) {
    section("6b. WebSocket mask enforcement, message cap, PING/PONG [C3, W-c]");
    // Sanity: masked frames are processed.
    {
        wstest::Client c;
        CHECK(c.connect_ws(port, "u1"));
        CHECK(c.send_frame(0x1, "echo:hi"));
        std::string payload;
        int op = 0;
        CHECK(ws_read_text(c, payload, op));
        CHECK(op == 0x1 && payload == "echo:hi");
        // PING is answered with a PONG carrying the same payload.
        CHECK(c.send_frame(0x9, "png"));
        CHECK(ws_read_text(c, payload, op));
        CHECK(op == 0xA && payload == "png");
        // An unsolicited PONG is never answered: the next frame is the echo.
        CHECK(c.send_frame(0xA, "unsolicited"));
        CHECK(c.send_frame(0x1, "echo:after-pong"));
        CHECK(ws_read_text(c, payload, op));
        CHECK(op == 0x1 && payload == "echo:after-pong");
    }
    // Unmasked client frame: the server closes the connection.
    {
        wstest::Client c;
        CHECK(c.connect_ws(port, "u2"));
        CHECK(c.send_frame(0x1, "echo:unmasked", true, false));
        CHECK(c.wait_closed(3000));
    }
    // Fragmented message cap.
    {
        myframe::set_max_ws_message_bytes(4096);
        CHECK(myframe::max_ws_message_bytes() == 4096);
        {
            wstest::Client c;
            CHECK(c.connect_ws(port, "u3"));
            std::string part1 = "echo:" + std::string(1995, 'a');
            std::string part2(2000, 'b');
            CHECK(c.send_frame(0x1, part1, false));
            CHECK(c.send_frame(0x0, part2, true));
            std::string payload;
            int op = 0;
            CHECK(ws_read_text(c, payload, op));
            CHECK(payload == part1 + part2);
        }
        {
            wstest::Client c;
            CHECK(c.connect_ws(port, "u4"));
            CHECK(c.send_frame(0x1, std::string(1500, 'x'), false));
            CHECK(c.send_frame(0x0, std::string(1500, 'y'), false));
            CHECK(c.send_frame(0x0, std::string(1500, 'z'), false));
            CHECK(c.send_frame(0x0, std::string(1500, 'w'), true));
            CHECK(c.wait_closed(3000));
        }
        myframe::set_max_ws_message_bytes(0);
        CHECK(myframe::max_ws_message_bytes() == myframe::kDefaultMaxWsMessageBytes);
    }
    // 20,000 tiny masked frames packed into large buffers.
    {
        wstest::Client c;
        CHECK(c.connect_ws(port, "u5"));
        g_ws_counted.store(0);
        const int kFrames = 20000;
        std::string burst;
        burst.reserve(static_cast<size_t>(kFrames) * 8 + 16);
        for (int i = 0; i < kFrames; ++i) {
            burst += wstest::Client::build_frame(0x1, "ab", true, true, 0xa0b0c0d0u ^ static_cast<uint32_t>(i));
        }
        burst += wstest::Client::build_frame(0x1, "END", true, true);
        const uint64_t t0 = now_ms();
        CHECK(c.send_raw(burst));
        std::string payload;
        int op = 0;
        CHECK(ws_read_text(c, payload, op));
        const uint64_t elapsed = now_ms() - t0;
        std::printf("   server parsed %s frames in %llu ms\n", payload.c_str(),
                    static_cast<unsigned long long>(elapsed));
        CHECK(payload == std::to_string(static_cast<long long>(kFrames)));
        CHECK(elapsed < 1000);
    }
}

// ===========================================================================
// 7. TLS exact protocol range [T-a]
// ===========================================================================

static bool tls_handshake(unsigned short port, long max_version, bool do_request, std::string* status) {
    SSL_CTX* cctx = SSL_CTX_new(TLS_client_method());
    if (!cctx) return false;
    SSL_CTX_set_verify(cctx, SSL_VERIFY_NONE, NULL);
    if (max_version) SSL_CTX_set_max_proto_version(cctx, static_cast<int>(max_version));
    bool ok = false;
    int fd = tcp_connect(port, 3000);
    if (fd >= 0) {
        SSL* ssl = SSL_new(cctx);
        SSL_set_fd(ssl, fd);
        if (SSL_connect(ssl) == 1) {
            ok = true;
            if (do_request) {
                const std::string req = "GET /tls HTTP/1.1\r\nHost: t\r\n\r\n";
                ok = SSL_write(ssl, req.data(), static_cast<int>(req.size())) == static_cast<int>(req.size());
                std::string resp;
                char buf[4096];
                while (ok && resp.find("\r\n\r\n") == std::string::npos) {
                    int n = SSL_read(ssl, buf, sizeof(buf));
                    if (n <= 0) {
                        ok = false;
                        break;
                    }
                    resp.append(buf, static_cast<size_t>(n));
                }
                if (ok && status) *status = resp.substr(0, resp.find("\r\n"));
            }
            SSL_shutdown(ssl);
        } else {
            ERR_clear_error();
        }
        SSL_free(ssl);
        ::close(fd);
    }
    SSL_CTX_free(cctx);
    return ok;
}

static void test_tls_protocol_range(unsigned short port) {
    section("7. TLS \"TLSv1.3\" exact protocol range [T-a]");
    long minv = 0, maxv = 0;
    CHECK(myframe_parse_tls_protocols("TLSv1.3", minv, maxv));
    CHECK(minv == TLS1_3_VERSION && maxv == TLS1_3_VERSION);
    CHECK(myframe_parse_tls_protocols("TLSv1.2,TLSv1.3", minv, maxv));
    CHECK(minv == TLS1_2_VERSION && maxv == TLS1_3_VERSION);
    CHECK(myframe_parse_tls_protocols(" TLSv1.3 ; tlsv1.2 ", minv, maxv));
    CHECK(minv == TLS1_2_VERSION && maxv == TLS1_3_VERSION);
    CHECK(myframe_parse_tls_protocols("TLSv1.2:TLSv1.1", minv, maxv));
    CHECK(minv == TLS1_1_VERSION && maxv == TLS1_2_VERSION);
    CHECK(myframe_parse_tls_protocols("TLSv1", minv, maxv));
    CHECK(minv == TLS1_VERSION && maxv == TLS1_VERSION);
    CHECK(!myframe_parse_tls_protocols("TLSv1.3x", minv, maxv));
    CHECK(!myframe_parse_tls_protocols("", minv, maxv));

    std::string status;
    CHECK(!tls_handshake(port, TLS1_2_VERSION, false, NULL));
    CHECK(tls_handshake(port, 0, true, &status));
    CHECK(status == "HTTP/1.1 200 OK");
    CHECK(tls_handshake(port, TLS1_3_VERSION, false, NULL));
}

// ===========================================================================
// 8. HTTP/2 split frames, :status mapping, container tick mutation
// ===========================================================================

static std::string h2_frame(uint8_t type, uint8_t flags, uint32_t sid, const std::string& payload) {
    std::string f;
    const uint32_t len = static_cast<uint32_t>(payload.size());
    f.push_back(static_cast<char>((len >> 16) & 0xff));
    f.push_back(static_cast<char>((len >> 8) & 0xff));
    f.push_back(static_cast<char>(len & 0xff));
    f.push_back(static_cast<char>(type));
    f.push_back(static_cast<char>(flags));
    f.push_back(static_cast<char>((sid >> 24) & 0x7f));
    f.push_back(static_cast<char>((sid >> 16) & 0xff));
    f.push_back(static_cast<char>((sid >> 8) & 0xff));
    f.push_back(static_cast<char>(sid & 0xff));
    f += payload;
    return f;
}

// Detached connection stub (no container, no fd) for processes that need a
// live base_net_obj at construction time.
class DetachedConn : public base_net_obj {
public:
    void event_process(int) override {}
    int real_net_process() override { return 0; }
};

static int h2_status_for_index(uint8_t idx) {
    std::shared_ptr<base_net_obj> conn(new DetachedConn());
    http2_client_process c(conn, "GET", "http", "t", "/",
                           std::map<std::string, std::string>(), std::string());
    std::string block(1, static_cast<char>(0x80 | idx));
    std::string wire = h2_frame(0x4, 0, 0, "") + h2_frame(0x1, 0x5, 1, block);
    c.process_recv_buf(wire.data(), wire.size());
    for (;;) {
        std::string* s = c.get_send_buf();
        if (!s) break;
        delete s;
    }
    return c.status();
}

class TickChild : public base_net_obj {
public:
    TickChild() : calls(0), erased(false), calls_after_erase(0) {}
    void event_process(int) override {}
    int real_net_process() override {
        ++calls;
        if (erased) ++calls_after_erase;
        return 0;
    }
    int calls;
    bool erased;
    int calls_after_erase;
};

class TickSpawner : public base_net_obj {
public:
    TickSpawner() : ticks(0) {}
    void event_process(int) override {}
    int real_net_process() override {
        ++ticks;
        // Insert enough objects to force several rehashes of _obj_net_map
        // while obj_process() is walking it.
        for (int i = 0; i < 128; ++i) {
            std::shared_ptr<TickChild> c(new TickChild());
            std::shared_ptr<base_net_obj> base = c;
            base->set_net_container(get_net_container());
            base->set_real_net(true);
            get_net_container()->push_real_net(base);
            children.push_back(c);
        }
        // Erase an object that is part of the current snapshot.
        if (victim && !victim->erased) {
            victim->erased = true;
            get_net_container()->erase(victim->get_id()._id);
        }
        return 0;
    }
    int ticks;
    std::shared_ptr<TickChild> victim;
    std::vector<std::shared_ptr<TickChild> > children;
};

static void test_http2_and_container() {
    section("8. HTTP/2 split frames, :status mapping, tick mutation [N3, N21, N10]");
    // Server: preface + SETTINGS + PING split across many reads.
    {
        std::shared_ptr<base_net_obj> no_conn;
        http2_process p(no_conn);
        std::string wire(h2::CONNECTION_PREFACE, h2::CONNECTION_PREFACE_LEN);
        wire += h2_frame(0x4, 0, 0, "");
        wire += h2_frame(0x6, 0, 0, "12345678");
        const size_t cuts[] = {5, 24, 30, 33, 40, 45, wire.size()};
        std::string pending;
        size_t off = 0;
        bool all_len = true;
        for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); ++i) {
            std::string chunk = wire.substr(off, cuts[i] - off);
            off = cuts[i];
            size_t expect = pending.size() + chunk.size();
            if (feed_like_connection(p, pending, chunk) != expect) all_len = false;
        }
        CHECK(all_len);
        CHECK(pending.empty());
        std::string out;
        for (;;) {
            std::string* s = p.get_send_buf();
            if (!s) break;
            out += *s;
            delete s;
        }
        int ping_acks = 0, goaways = 0, settings_acks = 0;
        std::string ping_payload;
        size_t o = 0;
        while (out.size() - o >= 9) {
            const unsigned char* f = reinterpret_cast<const unsigned char*>(out.data() + o);
            uint32_t len = (static_cast<uint32_t>(f[0]) << 16) | (static_cast<uint32_t>(f[1]) << 8) | f[2];
            if (out.size() - o < 9 + len) break;
            if (f[3] == 0x6 && (f[4] & 0x1)) {
                ++ping_acks;
                ping_payload = out.substr(o + 9, len);
            }
            if (f[3] == 0x7) ++goaways;
            if (f[3] == 0x4 && (f[4] & 0x1)) ++settings_acks;
            o += 9 + len;
        }
        CHECK(o == out.size());
        CHECK(ping_acks == 1);
        CHECK(ping_payload == "12345678");
        CHECK(settings_acks == 1);
        CHECK(goaways == 0);
    }
    // Client static-table :status mapping (indices 8..14).
    CHECK(h2_status_for_index(8) == 200);
    CHECK(h2_status_for_index(9) == 204);
    CHECK(h2_status_for_index(10) == 206);
    CHECK(h2_status_for_index(11) == 304);
    CHECK(h2_status_for_index(12) == 400);
    CHECK(h2_status_for_index(13) == 404);
    CHECK(h2_status_for_index(14) == 500);

    // Container: objects inserted / erased during real_net_process().
    {
        common_obj_container container(77);
        std::shared_ptr<TickSpawner> spawner(new TickSpawner());
        std::shared_ptr<base_net_obj> sb = spawner;
        sb->set_net_container(&container);
        sb->set_real_net(true);
        container.push_real_net(sb);
        std::shared_ptr<TickChild> victim(new TickChild());
        {
            std::shared_ptr<base_net_obj> vb = victim;
            vb->set_net_container(&container);
            vb->set_real_net(true);
            container.push_real_net(vb);
        }
        spawner->victim = victim;
        const int kTicks = 4;
        for (int t = 0; t < kTicks; ++t) {
            container.obj_process();
        }
        CHECK(spawner->ticks == kTicks);
        CHECK(spawner->children.size() == static_cast<size_t>(kTicks) * 128u);
        CHECK(victim->calls_after_erase == 0);
        // Children inserted during tick k are processed from tick k+1 on.
        bool processed_ok = true;
        for (size_t i = 0; i < spawner->children.size(); ++i) {
            const int born_tick = static_cast<int>(i / 128u);  // 0-based
            const int expected = kTicks - 1 - born_tick;
            if (spawner->children[i]->calls != expected) processed_ok = false;
        }
        CHECK(processed_ok);
        spawner->children.clear();
        spawner->victim.reset();
    }
}

// ===========================================================================
// main
// ===========================================================================

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    // Defaults must be deterministic regardless of the caller's environment.
    ::unsetenv("MYFRAME_MAX_HTTP_BODY_BYTES");
    ::unsetenv("MYFRAME_MAX_WS_MESSAGE_BYTES");
    CHECK(myframe::max_http_body_bytes() == myframe::kDefaultMaxHttpBodyBytes);
    CHECK(myframe::max_ws_message_bytes() == myframe::kDefaultMaxWsMessageBytes);

    // Pure parser / unit scenarios.
    test_http_smuggling_and_framing();
    test_chunked_overflow();
    test_status_line_unit();
    test_binary_adapters();
    test_ws_header_parse_speed();
    test_http2_and_container();

    // TLS configured as exactly TLS 1.3. Because TLS is configured, the
    // unified factory always runs ProtocolDetector, even for plain HTTP.
    ssl_config conf;
    conf._cert_file = MYFRAME_TEST_CERT;
    conf._key_file = MYFRAME_TEST_KEY;
    conf._protocols = "TLSv1.3";
    conf._alpn = "http/1.1";
    tls_set_server_config(conf);

    CtxHandler ctx_handler;
    WsApp ws_app;
    std::shared_ptr<myframe::UnifiedProtocolFactory> factory(new myframe::UnifiedProtocolFactory());
    factory->register_ws_handler(&ws_app);
    factory->register_http_context_handler(&ctx_handler);
    const unsigned short port = wstest::pick_free_port();
    server srv(2);
    srv.bind("127.0.0.1", port);
    srv.set_business_factory(factory);
    srv.start();

    LegacyApp legacy_app;
    std::shared_ptr<MultiProtocolFactory> legacy_factory(
        new MultiProtocolFactory(&legacy_app, MultiProtocolFactory::Mode::PlainOnly));
    const unsigned short legacy_port = wstest::pick_free_port();
    server legacy_srv(1);
    legacy_srv.bind("127.0.0.1", legacy_port);
    legacy_srv.set_business_factory(legacy_factory);
    legacy_srv.start();
    sleep_ms(200);

    test_keepalive_status_line(port);
    test_detector_handoff(port, legacy_port);
    test_ws_hardening(port);
    test_tls_protocol_range(port);

    legacy_srv.stop();
    srv.stop();

    if (g_fail.load() == 0) {
        std::printf("test_security_and_framing: ALL PASS (%d checks)\n", g_checks.load());
        return 0;
    }
    std::printf("test_security_and_framing: %d of %d checks FAILED\n", g_fail.load(), g_checks.load());
    return 1;
}
