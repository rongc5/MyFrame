#include "client_conn_router.h"
#include "out_connect.h"
#include "http_req_process.h"
#include "http_client_data_process.h"
#include "web_socket_req_process.h"
#include "web_socket_data_process.h"
#include "app_ws_data_process.h"
#include "codec.h"
#include "client_ssl_codec.h"
#ifdef ENABLE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif
#include <regex>

namespace {
struct ParsedUrl { std::string scheme, host, port, path; };
static bool parse_url(const std::string& url, ParsedUrl& u) {
    std::regex re(R"((https?|wss?)://([^/ :]+)(:([0-9]+))?(/.*)?)");
    std::smatch m; if (!std::regex_match(url, m, re)) return false;
    u.scheme = m[1]; for (auto& c : u.scheme) c = (char)tolower(c);
    u.host   = m[2];
    u.port   = m[4].matched ? m[4].str() : ((u.scheme=="https"||u.scheme=="wss")?"443":"80");
    u.path   = m[5].matched ? m[5].str() : "/";
    return true;
}
}

namespace {
#ifdef ENABLE_SSL
template<class PROCESS>
class tls_out_connect : public out_connect<PROCESS> {
public:
    tls_out_connect(const std::string& ip, unsigned short port, const std::string& host)
        : out_connect<PROCESS>(ip, port), _host(host), _ssl_ctx(nullptr), _ssl(nullptr) {
        SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms();
        _ssl_ctx = SSL_CTX_new(TLS_client_method());
    }
    virtual ~tls_out_connect() {
        if (_ssl) { SSL_free(_ssl); _ssl = nullptr; }
        if (_ssl_ctx) { SSL_CTX_free(_ssl_ctx); _ssl_ctx = nullptr; }
    }
protected:
    virtual void connect_ok_process() override {
        out_connect<PROCESS>::connect_ok_process();
        if (!_ssl_ctx) return;
        _ssl = SSL_new(_ssl_ctx);
        SSL_set_tlsext_host_name(_ssl, _host.c_str());
        SSL_set_fd(_ssl, base_net_obj::_fd);
        this->set_codec(std::unique_ptr<ICodec>(new ClientSslCodec(_ssl)));
        // 交给 codec 进行非阻塞 SSL_connect，需要可写事件驱动
        this->update_event(this->get_event() | EPOLLOUT);
    }
private:
    std::string _host; SSL_CTX* _ssl_ctx; SSL* _ssl;
};
#endif
class DummyWsClientData : public web_socket_data_process {
public:
    explicit DummyWsClientData(web_socket_process* p) : web_socket_data_process(p) {}
    virtual void msg_recv_finish() override {}
};
}

static std::shared_ptr<base_net_obj> build_http_like(const std::string& url, const ClientBuildCtx& ctx, bool tls) {
    ParsedUrl u; if (!parse_url(url, u)) return nullptr;
    std::shared_ptr< out_connect<http_req_process> > conn;
#ifdef ENABLE_SSL
    if (tls) conn = std::make_shared< tls_out_connect<http_req_process> >(u.host, (unsigned short)atoi(u.port.c_str()), u.host);
    else
#endif
    conn = std::make_shared< out_connect<http_req_process> >(u.host, (unsigned short)atoi(u.port.c_str()));
    auto proc = new http_req_process(conn);
    proc->set_process(new http_client_data_process(proc, ctx.method, u.host, u.path, ctx.headers, ctx.body));
    conn->set_process(proc);
    if (ctx.container) conn->set_net_container(ctx.container);
    conn->connect();
    return std::static_pointer_cast<base_net_obj>(conn);
}

static std::shared_ptr<base_net_obj> build_ws_like(const std::string& url, const ClientBuildCtx& ctx, bool tls) {
    ParsedUrl u; if (!parse_url(url, u)) return nullptr;
    std::shared_ptr< out_connect<web_socket_req_process> > conn;
#ifdef ENABLE_SSL
    if (tls) conn = std::make_shared< tls_out_connect<web_socket_req_process> >(u.host, (unsigned short)atoi(u.port.c_str()), u.host);
    else
#endif
    conn = std::make_shared< out_connect<web_socket_req_process> >(u.host, (unsigned short)atoi(u.port.c_str()));
    auto proc = new web_socket_req_process(conn);
    proc->get_req_para()._s_path = u.path;
    proc->get_req_para()._s_host = u.host;
    proc->get_req_para()._origin = u.scheme + "://" + u.host;
    // 默认使用业务回调，否则使用占位数据处理器
    proc->set_process(ctx.handler ? (web_socket_data_process*)new app_ws_data_process(proc, ctx.handler)
                                  : (web_socket_data_process*)new DummyWsClientData(proc));
    conn->set_process(proc);
    if (ctx.container) conn->set_net_container(ctx.container);
    conn->connect();
    return std::static_pointer_cast<base_net_obj>(conn);
}

void register_default_client_builders(ClientConnRouter& r) {
    r.register_factory("http",  [](const std::string& url, const ClientBuildCtx& ctx){ return build_http_like(url, ctx, false); });
    r.register_factory("https", [](const std::string& url, const ClientBuildCtx& ctx){ return build_http_like(url, ctx, true ); });
    r.register_factory("ws",    [](const std::string& url, const ClientBuildCtx& ctx){ return build_ws_like(url,   ctx, false); });
    r.register_factory("wss",   [](const std::string& url, const ClientBuildCtx& ctx){ return build_ws_like(url,   ctx, true ); });
}


