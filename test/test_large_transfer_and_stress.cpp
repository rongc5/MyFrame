// Large-transfer, TLS, header-guard and concurrency stress regression.
//
// A single in-process MyFrame server (UnifiedProtocolFactory, plain HTTP and
// TLS on the same port) is exercised by blocking socket clients:
//   1. Plain HTTP large Connection: close responses read by a slow reader.
//   2. Plain HTTP keep-alive sequential large responses on one connection.
//   3. HTTPS large Connection: close responses read by a slow TLS reader.
//   4. Mid-stream RST / FIN client disconnects (HTTP and HTTPS), followed by
//      healthy requests and a file-descriptor leak check.
//   5. Oversized request header rejection (MAX_HTTP_HEAD_LEN).
//   6. Outbound high-water-mark configuration boundaries.
//   7. High-concurrency mixed-size echo stress over keep-alive and close.
// Every response body is verified by exact length and FNV-1a 64-bit checksum.
#include "server.h"
#include "ssl_context.h"
#include "unified_protocol_factory.h"
#include "outbound_guard.h"
#include "base_def.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

static std::atomic<int> g_fail{0};
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                  \
            std::fflush(stdout);                                              \
            g_fail.fetch_add(1);                                              \
        }                                                                     \
    } while (0)

static const size_t KiB = 1024;
static const size_t MiB = 1024 * 1024;

// ---------------------------------------------------------------------------
// Deterministic payloads and checksums
// ---------------------------------------------------------------------------

static inline unsigned char pattern_byte(uint64_t i, uint32_t seed) {
    uint64_t x = (i + 1) * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(seed) * 0xC2B2AE3D27D4EB4Full);
    x ^= x >> 29;
    return static_cast<unsigned char>(x >> 17);
}

static void fill_pattern(std::string& out, size_t size, uint32_t seed) {
    out.resize(size);
    for (size_t i = 0; i < size; ++i) out[i] = static_cast<char>(pattern_byte(i, seed));
}

static const uint64_t kFnvOffset = 1469598103934665603ull;
static const uint64_t kFnvPrime = 1099511628211ull;

static inline uint64_t fnv1a_update(uint64_t h, const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= kFnvPrime;
    }
    return h;
}

static uint64_t expected_fnv(size_t size, uint32_t seed) {
    uint64_t h = kFnvOffset;
    for (size_t i = 0; i < size; ++i) {
        h ^= pattern_byte(i, seed);
        h *= kFnvPrime;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Server-side handler
// ---------------------------------------------------------------------------

static std::atomic<uint64_t> g_srv_echo_requests{0};
static std::atomic<uint64_t> g_srv_echo_bytes{0};

static size_t query_number(const std::string& url, const char* key) {
    std::string needle = std::string(key) + "=";
    size_t q = url.find('?');
    if (q == std::string::npos) return 0;
    size_t p = url.find(needle, q);
    if (p == std::string::npos) return 0;
    return static_cast<size_t>(std::strtoull(url.c_str() + p + needle.size(), nullptr, 10));
}

// Level-2 handler: HttpContextAdapter honours keep-alive (the Level-1
// IApplicationHandler adapter always answers Connection: close).
class StressHandler : public myframe::IProtocolHandler {
public:
    void on_http_request(myframe::HttpContext& ctx) override {
        const myframe::HttpRequest& req = ctx.request();
        myframe::HttpResponse& res = ctx.response();
        res.set_content_type("application/octet-stream");
        if (req.url.compare(0, 5, "/data") == 0) {
            size_t size = query_number(req.url, "size");
            uint32_t seed = static_cast<uint32_t>(query_number(req.url, "seed"));
            fill_pattern(res.body, size, seed);
        } else if (req.url.compare(0, 5, "/echo") == 0) {
            g_srv_echo_requests.fetch_add(1);
            g_srv_echo_bytes.fetch_add(req.body.size());
            res.body = req.body;
        } else {
            res.body = "ok";
        }
    }
};

// ---------------------------------------------------------------------------
// Client helpers
// ---------------------------------------------------------------------------

static unsigned short pick_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    unsigned short port = 18090;
    if (fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        socklen_t len = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port = ntohs(addr.sin_port);
        }
    }
    if (fd >= 0) ::close(fd);
    return port;
}

static int count_open_fds() {
    int n = 0;
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return -1;
    while (struct dirent* e = ::readdir(d)) {
        if (e->d_name[0] != '.') ++n;
    }
    ::closedir(d);
    return n - 1;  // exclude the descriptor used by opendir itself
}

static int tcp_connect(unsigned short port, int rcvbuf = 0, int timeout_sec = 15) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (rcvbuf > 0) {
        // Must be set before connect() so the advertised window stays small.
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }
    timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static SSL_CTX* g_client_ctx = nullptr;

// Blocking client connection, plain TCP or TLS.
class Conn {
public:
    Conn() {}
    ~Conn() { close_graceful(); }
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    bool open(unsigned short port, bool tls, int rcvbuf = 0) {
        _fd = tcp_connect(port, rcvbuf);
        if (_fd < 0) return false;
        if (!tls) return true;
        _ssl = SSL_new(g_client_ctx);
        if (!_ssl) return false;
        SSL_set_fd(_ssl, _fd);
        ERR_clear_error();
        int rc = SSL_connect(_ssl);
        if (rc != 1) {
            int err = SSL_get_error(_ssl, rc);
            int saved_errno = errno;
            char detail[256] = {0};
            unsigned long e = ERR_get_error();
            if (e) ERR_error_string_n(e, detail, sizeof(detail));
            std::printf("  client SSL_connect failed: rc=%d ssl_error=%d errno=%d (%s) detail=%s\n",
                        rc, err, saved_errno, std::strerror(saved_errno), detail);
            return false;
        }
        return true;
    }

    int fd() const { return _fd; }

    bool write_all(const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            if (_ssl) {
                ERR_clear_error();
                int n = SSL_write(_ssl, data.data() + off, static_cast<int>(data.size() - off));
                if (n <= 0) return false;
                off += static_cast<size_t>(n);
            } else {
                ssize_t n = ::send(_fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
                if (n <= 0) {
                    if (n < 0 && errno == EINTR) continue;
                    return false;
                }
                off += static_cast<size_t>(n);
            }
        }
        return true;
    }

    // Returns >0 bytes, 0 on orderly EOF, -1 on error / timeout.
    ssize_t read_some(char* buf, size_t len) {
        if (!_pending.empty()) {
            size_t n = std::min(len, _pending.size());
            std::memcpy(buf, _pending.data(), n);
            _pending.erase(0, n);
            return static_cast<ssize_t>(n);
        }
        if (_ssl) {
            ERR_clear_error();
            int n = SSL_read(_ssl, buf, static_cast<int>(len));
            if (n > 0) return n;
            int err = SSL_get_error(_ssl, n);
            if (err == SSL_ERROR_ZERO_RETURN) return 0;
            // Peer closed the TCP stream without close_notify: treat as EOF.
            if (err == SSL_ERROR_SYSCALL && n == 0) return 0;
            return -1;
        }
        for (;;) {
            ssize_t n = ::recv(_fd, buf, len, 0);
            if (n < 0 && errno == EINTR) continue;
            return n;
        }
    }

    void unread(const char* data, size_t len) { _pending.insert(0, data, len); }

    // Orderly close for plain sockets; TLS close_notify is best effort.
    void close_graceful() {
        if (_ssl) {
            SSL_shutdown(_ssl);
            SSL_free(_ssl);
            _ssl = nullptr;
        }
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

    // Abortive close: SO_LINGER{1,0} makes close() emit RST.
    void close_rst() {
        if (_fd >= 0) {
            linger lg;
            lg.l_onoff = 1;
            lg.l_linger = 0;
            ::setsockopt(_fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        }
        drop_ssl();
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

    // Half-close with FIN in the middle of a transfer, then release the fd.
    void close_fin() {
        drop_ssl();
        if (_fd >= 0) {
            ::shutdown(_fd, SHUT_WR);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ::close(_fd);
            _fd = -1;
        }
    }

private:
    void drop_ssl() {
        if (_ssl) {
            // Free without close_notify to emulate an abrupt client exit.
            SSL_set_quiet_shutdown(_ssl, 1);
            SSL_free(_ssl);
            _ssl = nullptr;
        }
    }

    int _fd{-1};
    SSL* _ssl{nullptr};
    std::string _pending;
};

struct Response {
    bool ok{false};
    int status{-1};
    std::string head;
    size_t content_length{0};
    bool has_content_length{false};
    size_t body_bytes{0};
    uint64_t fnv{kFnvOffset};
    bool saw_eof_after_body{false};
};

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct ReadOptions {
    size_t chunk{64 * KiB};
    int sleep_us{0};           // pause after each read to emulate a slow reader
    bool expect_eof{false};    // Connection: close: require server EOF after body
    size_t stop_after{0};      // >0: stop reading once this many body bytes arrived
};

static Response read_response(Conn& c, const ReadOptions& opt) {
    Response r;
    std::vector<char> buf(opt.chunk);
    std::string head;
    size_t he = std::string::npos;
    while (he == std::string::npos) {
        ssize_t n = c.read_some(buf.data(), buf.size());
        if (n <= 0) return r;
        head.append(buf.data(), static_cast<size_t>(n));
        he = head.find("\r\n\r\n");
        if (head.size() > 64 * KiB && he == std::string::npos) return r;
    }
    std::string extra = head.substr(he + 4);
    head.resize(he + 4);
    r.head = head;
    size_t sp = head.find(' ');
    if (sp != std::string::npos) r.status = std::atoi(head.c_str() + sp + 1);
    std::string lh = lower(head);
    size_t p = lh.find("\r\ncontent-length:");
    if (p != std::string::npos) {
        r.has_content_length = true;
        r.content_length = static_cast<size_t>(std::strtoull(head.c_str() + p + 17, nullptr, 10));
    }
    size_t take = std::min(extra.size(), r.content_length);
    r.fnv = fnv1a_update(r.fnv, extra.data(), take);
    r.body_bytes = take;
    if (extra.size() > take) c.unread(extra.data() + take, extra.size() - take);

    while (r.body_bytes < r.content_length) {
        if (opt.stop_after && r.body_bytes >= opt.stop_after) {
            r.ok = true;
            return r;
        }
        size_t want = std::min(buf.size(), r.content_length - r.body_bytes);
        ssize_t n = c.read_some(buf.data(), want);
        if (n <= 0) return r;
        r.fnv = fnv1a_update(r.fnv, buf.data(), static_cast<size_t>(n));
        r.body_bytes += static_cast<size_t>(n);
        if (opt.sleep_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(opt.sleep_us));
    }
    if (opt.expect_eof) {
        char t[256];
        ssize_t n = c.read_some(t, sizeof(t));
        r.saw_eof_after_body = (n == 0);
    }
    r.ok = r.has_content_length && r.body_bytes == r.content_length;
    return r;
}

static std::string get_request(const std::string& path, bool keep_alive) {
    return "GET " + path + " HTTP/1.1\r\nHost: stress\r\nConnection: " +
           (keep_alive ? "keep-alive" : "close") + "\r\n\r\n";
}

static std::string data_path(size_t size, uint32_t seed) {
    return "/data?size=" + std::to_string(size) + "&seed=" + std::to_string(seed);
}

static bool verify_body(const Response& r, size_t size, uint32_t seed, const char* tag) {
    bool ok = r.ok && r.status == 200 && r.content_length == size && r.body_bytes == size &&
              r.fnv == expected_fnv(size, seed);
    if (!ok) {
        std::printf("  [%s] mismatch: ok=%d status=%d cl=%zu bytes=%zu expected=%zu fnv=%016llx\n",
                    tag, r.ok ? 1 : 0, r.status, r.content_length, r.body_bytes, size,
                    static_cast<unsigned long long>(r.fnv));
    }
    return ok;
}

// One healthy small request on a fresh connection.
static bool healthy_request(unsigned short port, bool tls) {
    Conn c;
    if (!c.open(port, tls)) return false;
    if (!c.write_all(get_request(data_path(1234, 7), false))) return false;
    ReadOptions opt;
    opt.expect_eof = true;
    Response r = read_response(c, opt);
    return verify_body(r, 1234, 7, tls ? "healthy-tls" : "healthy-http");
}

static bool wait_for_fds_at_most(int limit, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (count_open_fds() <= limit) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return count_open_fds() <= limit;
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

static void scenario1_http_close_slow_reader(unsigned short port) {
    std::printf("[1] plain HTTP Connection: close, slow reader\n");
    const size_t sizes[] = {1536 * KiB, 8 * MiB, 32 * MiB};
    uint32_t seed = 11;
    for (size_t size : sizes) {
        Conn c;
        CHECK(c.open(port, false, 16384));
        CHECK(c.write_all(get_request(data_path(size, seed), false)));
        ReadOptions opt;
        opt.chunk = 8 * KiB;
        opt.sleep_us = (size <= 8 * MiB) ? 300 : 100;
        opt.expect_eof = true;
        Response r = read_response(c, opt);
        CHECK(verify_body(r, size, seed, "s1"));
        CHECK(r.saw_eof_after_body);
        std::printf("    %zu bytes ok\n", r.body_bytes);
        ++seed;
    }
}

static void scenario2_http_keepalive_sequential(unsigned short port) {
    std::printf("[2] plain HTTP keep-alive sequential large responses\n");
    const size_t sizes[] = {4 * MiB, 16 * MiB, 1 * MiB};
    Conn c;
    CHECK(c.open(port, false));
    uint32_t seed = 21;
    for (size_t size : sizes) {
        CHECK(c.write_all(get_request(data_path(size, seed), true)));
        ReadOptions opt;
        Response r = read_response(c, opt);
        CHECK(verify_body(r, size, seed, "s2"));
        CHECK(lower(r.head).find("connection: close") == std::string::npos);
        std::printf("    %zu bytes ok\n", r.body_bytes);
        ++seed;
    }
}

static void scenario3_https_close_slow_reader(unsigned short port) {
    std::printf("[3] HTTPS Connection: close, slow TLS reader\n");
    const size_t sizes[] = {2 * MiB, 8 * MiB};
    uint32_t seed = 31;
    for (size_t size : sizes) {
        Conn c;
        CHECK(c.open(port, true, 16384));
        CHECK(c.write_all(get_request(data_path(size, seed), false)));
        ReadOptions opt;
        opt.chunk = 4 * KiB;
        opt.sleep_us = 100;
        opt.expect_eof = true;
        Response r = read_response(c, opt);
        CHECK(verify_body(r, size, seed, "s3"));
        CHECK(r.saw_eof_after_body);
        std::printf("    %zu bytes ok\n", r.body_bytes);
        ++seed;
    }
}

static void scenario4_abrupt_disconnects(unsigned short port, int fd_baseline) {
    std::printf("[4] mid-stream RST / FIN disconnects\n");
    struct Case { bool tls; bool rst; const char* name; };
    const Case cases[] = {
        {false, true, "http-rst"}, {false, false, "http-fin"},
        {true, true, "https-rst"}, {true, false, "https-fin"},
    };
    uint32_t seed = 41;
    for (const Case& tc : cases) {
        for (int round = 0; round < 2; ++round) {
            Conn c;
            CHECK(c.open(port, tc.tls, 16384));
            CHECK(c.write_all(get_request(data_path(16 * MiB, seed), false)));
            ReadOptions opt;
            opt.chunk = 16 * KiB;
            opt.stop_after = 512 * KiB + static_cast<size_t>(round) * MiB;
            Response r = read_response(c, opt);
            CHECK(r.ok && r.status == 200 && r.content_length == 16 * MiB);
            CHECK(r.body_bytes >= opt.stop_after && r.body_bytes < 16 * MiB);
            if (tc.rst) c.close_rst(); else c.close_fin();
            ++seed;
        }
        CHECK(healthy_request(port, false));
        CHECK(healthy_request(port, true));
        bool fds_ok = wait_for_fds_at_most(fd_baseline, 20000);
        if (!fds_ok) {
            std::printf("  [%s] fd leak: open=%d baseline=%d\n", tc.name, count_open_fds(), fd_baseline);
        }
        CHECK(fds_ok);
        std::printf("    %s ok (fds=%d baseline=%d)\n", tc.name, count_open_fds(), fd_baseline);
    }
}

// Sends an oversized header and returns true when the server rejects it by
// closing the connection (EOF/RST) or with a non-2xx status, without waiting
// for the client receive timeout. With after_valid_request the connection is
// first used for a normal keep-alive request, so the oversized header reaches
// the HTTP parser (http_base_process) rather than the protocol detector.
static bool oversized_header_rejected(unsigned short port, bool with_terminator,
                                      bool after_valid_request) {
    Conn c;
    if (!c.open(port, false)) return false;
    timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    ::setsockopt(c.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (after_valid_request) {
        if (!c.write_all(get_request(data_path(4096, 5), true))) return false;
        ReadOptions opt;
        Response r = read_response(c, opt);
        if (!verify_body(r, 4096, 5, "s5-prelude")) return false;
    }
    std::string req = "GET /data?size=16&seed=1 HTTP/1.1\r\nHost: stress\r\n";
    req += "X-Filler: " + std::string(110 * KiB, 'a') + "\r\n";
    if (with_terminator) req += "\r\n";
    size_t off = 0;
    while (off < req.size()) {
        ssize_t n = ::send(c.fd(), req.data() + off, req.size() - off, MSG_NOSIGNAL);
        if (n <= 0) break;  // the server may already have reset the connection
        off += static_cast<size_t>(n);
    }
    std::string resp;
    bool closed = false;
    bool timed_out = false;
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(c.fd(), buf, sizeof(buf), 0);
        if (n > 0) { resp.append(buf, static_cast<size_t>(n)); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) timed_out = true;
        else closed = true;
        break;
    }
    int status = -1;
    if (resp.compare(0, 5, "HTTP/") == 0) {
        size_t sp = resp.find(' ');
        if (sp != std::string::npos) status = std::atoi(resp.c_str() + sp + 1);
    }
    bool rejected = closed && !timed_out && (status < 0 || status >= 400);
    if (!rejected) {
        std::printf("  oversized header (terminator=%d, after_valid=%d) not rejected: "
                    "status=%d closed=%d timeout=%d\n",
                    with_terminator ? 1 : 0, after_valid_request ? 1 : 0, status,
                    closed ? 1 : 0, timed_out ? 1 : 0);
    }
    return rejected;
}

static void scenario5_oversized_header(unsigned short port) {
    std::printf("[5] oversized HTTP header rejection (MAX_HTTP_HEAD_LEN=%u)\n",
                static_cast<unsigned>(MAX_HTTP_HEAD_LEN));
    for (int after_valid = 0; after_valid < 2; ++after_valid) {
        CHECK(oversized_header_rejected(port, false, after_valid != 0));
        CHECK(oversized_header_rejected(port, true, after_valid != 0));
    }
    CHECK(healthy_request(port, false));
    std::printf("    ok\n");
}

static void scenario6_hwm_boundaries() {
    std::printf("[6] outbound high-water-mark boundaries\n");
    using myframe::detail::OutboundState;
    CHECK(myframe::kDefaultOutboundHighWaterMark == 4 * MiB);
    myframe::set_outbound_high_water_mark(0);
    CHECK(myframe::outbound_high_water_mark() == 4 * MiB);

    {
        // An idle connection always admits one payload, even above the HWM.
        OutboundState st;
        CHECK(st.limit() == 4 * MiB);
        CHECK(st.try_reserve(6 * MiB));
        CHECK(!st.try_reserve(1));
        st.release(6 * MiB);
        CHECK(st.queued_bytes.load() == 0);
    }
    {
        // Under the default HWM, chunks are admitted only up to 4 MiB total.
        OutboundState st;
        CHECK(st.try_reserve(2 * MiB));
        CHECK(st.try_reserve(2 * MiB));
        CHECK(!st.try_reserve(1));
    }

    myframe::set_outbound_high_water_mark(32 * MiB);
    CHECK(myframe::outbound_high_water_mark() == 32 * MiB);
    {
        OutboundState st;
        CHECK(st.limit() == 32 * MiB);
        for (int i = 0; i < 8; ++i) CHECK(st.try_reserve(4 * MiB));
        CHECK(st.queued_bytes.load() == 32 * MiB);
        CHECK(!st.try_reserve(1));
        st.release(4 * MiB);
        CHECK(st.try_reserve(4 * MiB));
        CHECK(!st.try_reserve(1));
    }
    {
        OutboundState st;
        CHECK(st.try_reserve(16 * MiB));
        CHECK(!st.try_reserve(16 * MiB + 1));
        CHECK(st.try_reserve(16 * MiB));
    }

    myframe::set_outbound_high_water_mark(0);
    CHECK(myframe::outbound_high_water_mark() == myframe::kDefaultOutboundHighWaterMark);
    {
        OutboundState st;
        CHECK(st.limit() == 4 * MiB);
        CHECK(st.try_reserve(4 * MiB));
        CHECK(!st.try_reserve(1));
    }
    std::printf("    ok\n");
}

static void scenario7_concurrency_stress(unsigned short port) {
    const int kThreads = 24;
    const int kConnsPerThread = 4;
    const int kRequestsPerKeepAlive = 3;
    std::printf("[7] %d-thread mixed-size HTTP echo stress\n", kThreads);

    std::atomic<uint64_t> sent_requests{0}, ok_responses{0}, sent_bytes{0}, recv_bytes{0};
    std::atomic<int> errors{0};
    const uint64_t srv_req_before = g_srv_echo_requests.load();
    const uint64_t srv_bytes_before = g_srv_echo_bytes.load();

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int k = 0; k < kConnsPerThread; ++k) {
                const bool keep_alive = ((t + k) % 2) == 0;
                const int requests = keep_alive ? kRequestsPerKeepAlive : 1;
                Conn c;
                if (!c.open(port, false)) { errors.fetch_add(1); continue; }
                for (int q = 0; q < requests; ++q) {
                    const bool large = ((t + k + q) % 3) == 0;
                    const size_t size = large ? 512 * KiB : 4 * KiB;
                    const uint32_t seed = static_cast<uint32_t>(1000 + t * 100 + k * 10 + q);
                    std::string body;
                    fill_pattern(body, size, seed);
                    std::string req = "POST /echo HTTP/1.1\r\nHost: stress\r\nContent-Type: application/octet-stream\r\n"
                                      "Content-Length: " + std::to_string(size) + "\r\nConnection: " +
                                      (keep_alive ? "keep-alive" : "close") + "\r\n\r\n" + body;
                    if (!c.write_all(req)) { errors.fetch_add(1); break; }
                    sent_requests.fetch_add(1);
                    sent_bytes.fetch_add(size);
                    ReadOptions opt;
                    opt.expect_eof = !keep_alive;
                    Response r = read_response(c, opt);
                    if (!verify_body(r, size, seed, "s7") || (!keep_alive && !r.saw_eof_after_body)) {
                        errors.fetch_add(1);
                        break;
                    }
                    ok_responses.fetch_add(1);
                    recv_bytes.fetch_add(r.body_bytes);
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    uint64_t expected_requests = 0, expected_bytes = 0;
    for (int t = 0; t < kThreads; ++t) {
        for (int k = 0; k < kConnsPerThread; ++k) {
            const bool keep_alive = ((t + k) % 2) == 0;
            const int requests = keep_alive ? kRequestsPerKeepAlive : 1;
            for (int q = 0; q < requests; ++q) {
                ++expected_requests;
                expected_bytes += ((t + k + q) % 3) == 0 ? 512 * KiB : 4 * KiB;
            }
        }
    }
    CHECK(errors.load() == 0);
    CHECK(sent_requests.load() == expected_requests);
    CHECK(ok_responses.load() == expected_requests);
    CHECK(sent_bytes.load() == expected_bytes);
    CHECK(recv_bytes.load() == expected_bytes);
    CHECK(g_srv_echo_requests.load() - srv_req_before == expected_requests);
    CHECK(g_srv_echo_bytes.load() - srv_bytes_before == expected_bytes);
    std::printf("    requests=%llu responses=%llu bytes=%llu errors=%d\n",
                static_cast<unsigned long long>(sent_requests.load()),
                static_cast<unsigned long long>(ok_responses.load()),
                static_cast<unsigned long long>(recv_bytes.load()), errors.load());
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    // The HWM default may come from the environment; make it deterministic.
    ::unsetenv("MYFRAME_OUTBOUND_HWM_BYTES");

    // Scenario 6 is pure configuration logic: run it before any traffic so
    // the temporary 32 MiB override cannot influence live connections.
    scenario6_hwm_boundaries();

    ssl_config conf;
    conf._cert_file = MYFRAME_TEST_CERT;
    conf._key_file = MYFRAME_TEST_KEY;
    conf._protocols = "TLSv1.2,TLSv1.3";
    conf._alpn = "http/1.1";
    tls_set_server_config(conf);

    g_client_ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(g_client_ctx, SSL_VERIFY_NONE, nullptr);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    // The server may close the TCP stream without close_notify after a
    // Connection: close response; report that as a clean EOF.
    SSL_CTX_set_options(g_client_ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif

    const unsigned short port = pick_free_port();
    StressHandler handler;
    std::shared_ptr<myframe::UnifiedProtocolFactory> factory(new myframe::UnifiedProtocolFactory());
    factory->register_http_context_handler(&handler);
    server srv(4);
    srv.bind("127.0.0.1", port);
    srv.set_business_factory(factory);
    srv.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Warm up both transports, then record the descriptor baseline.
    CHECK(healthy_request(port, false));
    CHECK(healthy_request(port, true));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int fd_baseline = count_open_fds();
    std::printf("fd baseline: %d\n", fd_baseline);

    scenario1_http_close_slow_reader(port);
    scenario2_http_keepalive_sequential(port);
    scenario3_https_close_slow_reader(port);
    scenario4_abrupt_disconnects(port, fd_baseline);
    scenario5_oversized_header(port);
    scenario7_concurrency_stress(port);

    CHECK(healthy_request(port, false));
    CHECK(healthy_request(port, true));
    bool fds_ok = wait_for_fds_at_most(fd_baseline, 20000);
    if (!fds_ok) std::printf("final fd count %d > baseline %d\n", count_open_fds(), fd_baseline);
    CHECK(fds_ok);

    srv.stop();
    SSL_CTX_free(g_client_ctx);

    if (g_fail.load() == 0) {
        std::printf("test_large_transfer_and_stress: ALL PASS\n");
        return 0;
    }
    std::printf("test_large_transfer_and_stress: %d FAIL\n", g_fail.load());
    return 1;
}
