#include "client_iface.h"
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

// OpenSSL 1.0.2 compatibility
#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define TLS_client_method() SSLv23_client_method()
#endif

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
    if (u.port.empty()) u.port = (u.scheme=="https"?"443":"80");
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
}

class Http1Client final : public IRequest {
public:
    Http1Client() {}
    ~Http1Client() override {}
    HttpResult get(const std::string& url, const std::map<std::string,std::string>& hdrs) override {
        ParsedUrl u = parse_url(url);
        int fd = tcp_connect(u.host, u.port); if (fd<0) return {};
        bool use_tls = (u.scheme=="https");
        SSL* ssl=nullptr; SSL_CTX* ctx=nullptr;
        if (use_tls) {
            SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms();
            ctx = SSL_CTX_new(TLS_client_method()); if (!ctx) { ::close(fd); return {}; }
            ssl = SSL_new(ctx); SSL_set_tlsext_host_name(ssl, u.host.c_str()); SSL_set_fd(ssl, fd);
            if (SSL_connect(ssl)!=1) { SSL_free(ssl); SSL_CTX_free(ctx); ::close(fd); return {}; }
        }
        std::string req = "GET "+u.path+" HTTP/1.1\r\nHost: "+u.host+"\r\nConnection: close\r\n";
        for (auto& kv:hdrs) req += kv.first+": "+kv.second+"\r\n";
        req += "\r\n";
        auto send_all = [&](const char* b, size_t n){ size_t off=0; while(off<n){ ssize_t w= use_tls? SSL_write(ssl,b+off,(int)(n-off)) : ::send(fd,b+off,n-off,0); if (w<=0) return false; off+= (size_t)w;} return true; };
        if (!send_all(req.c_str(), req.size())) { if (ssl) {SSL_free(ssl); SSL_CTX_free(ctx);} ::close(fd); return {}; }
        std::string resp; char buf[8192];
        while (true) {
            int r = use_tls? SSL_read(ssl, buf, sizeof buf) : ::recv(fd, buf, sizeof buf, 0);
            if (r<=0) break; resp.append(buf, r);
        }
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx);} ::close(fd);
        // very simple parse: split by CRLFCRLF
        HttpResult out; size_t p = resp.find("\r\n\r\n");
        std::string head = (p==std::string::npos)?resp:resp.substr(0,p);
        out.body = (p==std::string::npos)?std::string():resp.substr(p+4);
        // status line
        size_t eol = head.find("\r\n"); std::string status = head.substr(0, eol);
        size_t sp = status.find(' '); if (sp!=std::string::npos){ size_t sp2 = status.find(' ', sp+1); if (sp2!=std::string::npos) out.status = atoi(status.substr(sp+1, sp2-sp-1).c_str()); }
        // headers (not robust)
        size_t pos = (eol==std::string::npos)?head.size():eol+2;
        while (pos < head.size()) {
            size_t nl = head.find("\r\n", pos); std::string line = head.substr(pos, nl-pos);
            size_t col = line.find(':'); if (col!=std::string::npos){ std::string k=line.substr(0,col); std::string v=line.substr(col+1); if (!v.empty() && v[0]==' ') v.erase(0,1); out.headers[k]=v; }
            if (nl==std::string::npos) break; pos = nl+2;
        }
        return out;
    }
};

class Http1Factory final : public IRequestFactory {
public:
    std::unique_ptr<IRequest> create() override { return std::unique_ptr<IRequest>(new Http1Client()); }
};

// expose simple helpers
std::shared_ptr<IRequestFactory> make_http1_factory() { return std::shared_ptr<IRequestFactory>(new Http1Factory()); }

