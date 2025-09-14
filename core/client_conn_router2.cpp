#include "client_conn_router.h"
#include "out_connect.h"
#include "tls_out_connect.h"
#include "http_req_process.h"
#include "http2_client_process.h"
#include "http_client_data_process.h"
#include "hybrid_https_client_process.h"
#include "web_socket_req_process.h"
#include "web_socket_data_process.h"
#include "app_ws_data_process.h"
#include <regex>

namespace {
struct ParsedUrl { std::string scheme, host, port, path; };
static bool parse_url(const std::string& url, ParsedUrl& u) {
    std::regex re(R"((https?|wss?|h2)://([^/ :]+)(:([0-9]+))?(/.*)?)");
    std::smatch m; if (!std::regex_match(url, m, re)) return false;
    u.scheme = m[1]; for (auto& c : u.scheme) c = (char)tolower(c);
    u.host   = m[2];
    u.port   = m[4].matched ? m[4].str() : ((u.scheme=="https"||u.scheme=="wss")?"443":"80");
    u.path   = m[5].matched ? m[5].str() : "/";
    return true;
}

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
    proc->set_process(ctx.handler ? (web_socket_data_process*)new app_ws_data_process(proc, ctx.handler)
                                  : (web_socket_data_process*)new DummyWsClientData(proc));
    conn->set_process(proc);
    if (ctx.container) conn->set_net_container(ctx.container);
    conn->connect();
    return std::static_pointer_cast<base_net_obj>(conn);
}

void register_default_client_builders(ClientConnRouter& r) {
    r.register_factory("http",  [](const std::string& url, const ClientBuildCtx& ctx){ return build_http_like(url, ctx, false); });
    r.register_factory("https", [](const std::string& url, const ClientBuildCtx& ctx){
        ParsedUrl u; if (!parse_url(url, u)) return std::shared_ptr<base_net_obj>();
        // Build :authority (host[:port] when non-default)
        std::string authority = u.host;
        if (!u.port.empty() && u.port != "443") authority = u.host + ":" + u.port;
        std::shared_ptr< out_connect<hybrid_https_client_process> > conn;
        // Offer ALPN h2 first, then http/1.1
        conn = std::make_shared< tls_out_connect<hybrid_https_client_process> >(u.host, (unsigned short)atoi(u.port.c_str()), u.host, std::string("h2,http/1.1"));
        auto proc = new hybrid_https_client_process(conn, ctx.method, authority, u.path, ctx.headers, ctx.body);
        conn->set_process(proc);
        if (ctx.container) conn->set_net_container(ctx.container);
        conn->connect();
        return std::static_pointer_cast<base_net_obj>(conn);
    });
    r.register_factory("ws",    [](const std::string& url, const ClientBuildCtx& ctx){ return build_ws_like(url,   ctx, false); });
    r.register_factory("wss",   [](const std::string& url, const ClientBuildCtx& ctx){ return build_ws_like(url,   ctx, true ); });
    // HTTP/2 over TLS (explicit scheme h2://host[:port]/path)
    r.register_factory("h2",    [](const std::string& url, const ClientBuildCtx& ctx){
        ParsedUrl u; if (!parse_url(url, u)) return std::shared_ptr<base_net_obj>();
        std::string authority = u.host; if (!u.port.empty() && u.port != "443") authority = u.host + ":" + u.port;
        std::shared_ptr< out_connect<http2_client_process> > conn;
        // Force ALPN h2 for HTTP/2
        conn = std::make_shared< tls_out_connect<http2_client_process> >(u.host, (unsigned short)atoi(u.port.c_str()), u.host, std::string("h2"));
        // Over TLS + ALPN h2, the correct :scheme is "https"
        auto proc = new http2_client_process(conn, ctx.method, std::string("https"), authority, u.path, ctx.headers, ctx.body);
        conn->set_process(proc);
        if (ctx.container) conn->set_net_container(ctx.container);
        conn->connect();
        return std::static_pointer_cast<base_net_obj>(conn);
    });
}
