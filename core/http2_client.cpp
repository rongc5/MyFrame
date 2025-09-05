#include "client_iface.h"
#include "http2_frame.h"
#include <string>
#include <map>
#include <memory>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace {
struct ParsedUrl { std::string scheme, host, port, path; };
static ParsedUrl parse_url(const std::string& url) {
    ParsedUrl u; size_t p = url.find("://");
    u.scheme = (p==std::string::npos)?"":url.substr(0,p);
    size_t i = (p==std::string::npos)?0:p+3;
    size_t slash = url.find('/', i);
    std::string hostport = (slash==std::string::npos)?url.substr(i):url.substr(i, slash-i);
    size_t col = hostport.find(':');
    if (col==std::string::npos) { u.host=hostport; }
    else { u.host=hostport.substr(0,col); u.port=hostport.substr(col+1); }
    for (auto& c : u.scheme) c = (char)tolower(c);
    if (u.port.empty()) u.port = (u.scheme=="h2"?"443":"80");
    u.path = (slash==std::string::npos)?"/":url.substr(slash);
    return u;
}

static int tcp_connect(const std::string& host, const std::string& port) {
    struct addrinfo hints; memset(&hints,0,sizeof hints); hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res)!=0) return -1;
    int fd=-1; for (auto p=res;p;p=p->ai_next){ fd=::socket(p->ai_family,p->ai_socktype,p->ai_protocol); if (fd<0) continue; if (::connect(fd,p->ai_addr,p->ai_addrlen)==0) break; ::close(fd); fd=-1; }
    freeaddrinfo(res); return fd;
}

// Minimal HPACK encoder helpers (no huffman, literal without indexing, using indexed names where possible)
static void hpack_write_int(std::string& out, uint32_t value, int prefix_bits) {
    uint8_t max = (uint8_t)((1u<<prefix_bits)-1u);
    if (value < max) { out.push_back((char)value); return; }
    out.push_back((char)max);
    value -= max;
    while (value >= 128) { out.push_back((char)((value % 128) + 128)); value /= 128; }
    out.push_back((char)value);
}

static void hpack_write_str(std::string& out, const std::string& s) {
    // 1-bit Huffman flag = 0 (no), 7-bit length
    if (s.size() < 127) {
        out.push_back((char)s.size());
    } else {
        // 0xxxxxxx with 7-bit prefix int encoding
        uint8_t b = 0x7f; out.push_back((char)b);
        uint32_t remain = (uint32_t)s.size() - 127;
        while (remain >= 128) { out.push_back((char)((remain % 128) + 128)); remain /= 128; }
        out.push_back((char)remain);
    }
    out.append(s);
}

static void hpack_literal_indexed_name(std::string& out, uint32_t index, const std::string& value) {
    // 0000|index (4-bit prefix)
    uint8_t first = 0x00; out.push_back((char)first); // will overwrite low bits via hpack_write_int machinery
    // rewrite last pushed byte with proper value: pop then compute
    out.pop_back();
    // write integer with 4-bit prefix into a fresh byte
    // compute prefix-limited integer encoding
    uint8_t max = 0x0f; // 4 bits
    if (index < max) { out.push_back((char)((first & 0xF0) | (index & 0x0F))); }
    else {
        out.push_back((char)((first & 0xF0) | max));
        uint32_t v = index - max;
        while (v >= 128) { out.push_back((char)((v % 128) + 128)); v /= 128; }
        out.push_back((char)v);
    }
    hpack_write_str(out, value);
}

static std::string build_header_block(const ParsedUrl& u, const std::map<std::string,std::string>& headers) {
    // Use static table indices (RFC 7541 Appendix A):
    // 1 :authority, 2 :method GET, 6 :scheme http, 7 :scheme https, 4 :path /
    std::string blk;
    // :method GET
    hpack_literal_indexed_name(blk, 2, "GET");
    // :path
    hpack_literal_indexed_name(blk, 4, u.path);
    // :authority
    hpack_literal_indexed_name(blk, 1, u.host);
    // :scheme
    hpack_literal_indexed_name(blk, (u.scheme=="h2"?7:6), (u.scheme=="h2"?"https":"http"));
    // additional headers
    for (auto& kv : headers) {
        // write as new-name literal without indexing: 0000 0000 name value
        // First byte with 4-bit prefix zero then name string
        blk.push_back((char)0x00);
        hpack_write_str(blk, kv.first);
        hpack_write_str(blk, kv.second);
    }
    return blk;
}
}

class Http2Client final : public IRequest {
public:
    Http2Client() {}
    ~Http2Client() override {}
    HttpResult get(const std::string& url, const std::map<std::string,std::string>& hdrs) override {
        ParsedUrl u = parse_url(url);
        // Only TLS h2 (ALPN) supported here
        int fd = tcp_connect(u.host, u.port); if (fd<0) return {};
        SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms();
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method()); if (!ctx) { ::close(fd); return {}; }
        SSL* ssl = SSL_new(ctx); SSL_set_tlsext_host_name(ssl, u.host.c_str()); SSL_set_fd(ssl, fd);
        // Advertise ALPN "h2"
        const unsigned char alpn_protos[] = { 2, 'h', '2' };
        SSL_set_alpn_protos(ssl, alpn_protos, sizeof(alpn_protos));
        if (SSL_connect(ssl)!=1) { SSL_free(ssl); SSL_CTX_free(ctx); ::close(fd); return {}; }
        const unsigned char* sel = nullptr; unsigned int slen = 0; SSL_get0_alpn_selected(ssl, &sel, &slen);
        if (!(slen==2 && sel && sel[0]=='h' && sel[1]=='2')) {
            SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); ::close(fd); return {}; // no h2
        }

        auto send_all = [&](const std::string& s){ size_t off=0; while(off<s.size()){ int w=SSL_write(ssl, s.data()+off, (int)(s.size()-off)); if (w<=0) return false; off+=(size_t)w;} return true; };
        auto recv_n = [&](unsigned char* buf, size_t n){ size_t off=0; while(off<n){ int r=SSL_read(ssl, (char*)buf+off, (int)(n-off)); if (r<=0) return false; off+=(size_t)r;} return true; };

        // 1) client connection preface
        if (!send_all(std::string(h2::CONNECTION_PREFACE, h2::CONNECTION_PREFACE_LEN))) { SSL_free(ssl); SSL_CTX_free(ctx); ::close(fd); return {}; }
        // 2) SETTINGS (empty)
        if (!send_all(h2::make_settings_frame(false))) { SSL_free(ssl); SSL_CTX_free(ctx); ::close(fd); return {}; }

        // 3) Build and send HEADERS for stream 1 (END_HEADERS | END_STREAM)
        std::string block = build_header_block(u, hdrs);
        uint8_t flags = 0x4 /*END_HEADERS*/ | 0x1 /*END_STREAM*/;
        std::string hdr = h2::make_frame_header((uint32_t)block.size(), h2::HEADERS, flags, 1);
        if (!send_all(hdr + block)) { SSL_free(ssl); SSL_CTX_free(ctx); ::close(fd); return {}; }

        // 4) Read frames: reply SETTINGS (send ACK), then HEADERS/DATA for stream 1
        HttpResult out; out.status = 200; // minimal
        std::string body;
        bool done = false;
        while (!done) {
            unsigned char fh[9]; if (!recv_n(fh, 9)) break;
            uint32_t len = h2::read24(fh);
            uint8_t type = fh[3];
            uint8_t flags_in = fh[4];
            uint32_t sid = h2::read32(&fh[5]) & 0x7fffffffu;
            std::string payload; payload.resize(len);
            if (len>0) { if (!recv_n((unsigned char*)&payload[0], len)) break; }
            if (type == h2::SETTINGS) {
                if ((flags_in & h2::FLAGS_ACK) == 0) {
                    // reply ACK
                    if (!send_all(h2::make_settings_ack())) break;
                }
            } else if (type == h2::DATA && sid == 1) {
                body.append(payload);
                if (flags_in & 0x1 /*END_STREAM*/) done = true;
            } else if (type == h2::HEADERS && sid == 1) {
                if (flags_in & 0x1 /*END_STREAM*/) done = true;
                // skip header parse (status left as 200)
            } else if (type == h2::GOAWAY) {
                done = true; break;
            }
        }

        out.body = std::move(body);
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); ::close(fd);
        return out;
    }
};

class Http2Factory final : public IRequestFactory {
public:
    std::unique_ptr<IRequest> create() override { return std::unique_ptr<IRequest>(new Http2Client()); }
};

std::shared_ptr<IRequestFactory> make_http2_factory() { return std::shared_ptr<IRequestFactory>(new Http2Factory()); }


