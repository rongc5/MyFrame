#ifndef __PROTOCOL_PROBES_H__
#define __PROTOCOL_PROBES_H__

#include "app_handler.h"
#include "http_res_process.h"
#include "web_socket_res_process.h"
#include "app_http_data_process.h"
#include "app_ws_data_process.h"
#include "tls_entry_process.h"
#include "custom_stream_process.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

// IProtocolProbe 抽象
class IProtocolProbe {
public:
    virtual bool match(const char* buf, size_t len) const = 0;
    virtual std::unique_ptr<base_data_process>
        create(std::shared_ptr<base_net_obj> conn) const = 0;
    virtual ~IProtocolProbe() {}
protected:
    explicit IProtocolProbe(IAppHandler* app = 0) : _app(app) {}
    IAppHandler* _app;
};

class HttpProbe : public IProtocolProbe {
public:
    explicit HttpProbe(IAppHandler* app = 0) : IProtocolProbe(app) {}
    bool match(const char* buf, size_t len) const override {
        if (len < 3) return false;
        if (len >= 4 && (memcmp(buf, "GET ", 4) == 0 || memcmp(buf, "PUT ", 4) == 0)) return true;
        if (len >= 5 && (memcmp(buf, "POST ", 5) == 0 || memcmp(buf, "HEAD ", 5) == 0)) return true;
        if (len >= 7 && memcmp(buf, "DELETE ", 7) == 0) return true;
        if (len >= 8 && memcmp(buf, "OPTIONS ", 8) == 0) return true;
        return false;
    }
    std::unique_ptr<base_data_process>
    create(std::shared_ptr<base_net_obj> conn) const override {
        std::fprintf(stderr, "[detect] HttpProbe selected\n");
        std::unique_ptr<http_res_process> p(new http_res_process(conn));
        p->set_process(new app_http_data_process(p.get(), _app));
        return std::unique_ptr<base_data_process>(p.release());
    }
};

class WsProbe : public IProtocolProbe {
public:
    explicit WsProbe(IAppHandler* app = 0) : IProtocolProbe(app) {}
    bool match(const char* buf, size_t len) const override {
        if (len < 10) return false;
        std::string request(buf, len), lower = request;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        bool has_upgrade = (lower.find("upgrade: websocket") != std::string::npos);
        bool has_connection = (lower.find("connection:") != std::string::npos) && (lower.find("upgrade") != std::string::npos);
        bool has_key = (lower.find("sec-websocket-key:") != std::string::npos);
        return has_upgrade && has_connection && has_key;
    }
    std::unique_ptr<base_data_process>
    create(std::shared_ptr<base_net_obj> conn) const override {
        std::fprintf(stderr, "[detect] WsProbe selected (Upgrade: websocket)\n");
        std::unique_ptr<web_socket_res_process> p(new web_socket_res_process(conn));
        p->set_process(new app_ws_data_process(p.get(), _app));
        return std::unique_ptr<base_data_process>(p.release());
    }
};

class TlsProbe : public IProtocolProbe {
public:
    explicit TlsProbe(IAppHandler* app = 0) : IProtocolProbe(app) {}
    bool match(const char* buf, size_t len) const override {
        if (len < 3) return false;
        return buf[0] == 0x16 && buf[1] == 0x03 && (buf[2] >= 0x01 && buf[2] <= 0x04);
    }
    std::unique_ptr<base_data_process>
    create(std::shared_ptr<base_net_obj> conn) const override {
        std::fprintf(stderr, "[detect] TlsProbe selected (TLS handshake)\n");
        return std::unique_ptr<base_data_process>(new tls_entry_process(conn, _app));
    }
};

class CustomProbe : public IProtocolProbe {
public:
    explicit CustomProbe(IAppHandler* app = 0) : IProtocolProbe(app) {}
    bool match(const char* buf, size_t len) const override { return len >= 4 && (std::memcmp(buf, "CSTM", 4) == 0); }
    std::unique_ptr<base_data_process>
    create(std::shared_ptr<base_net_obj> conn) const override {
        std::fprintf(stderr, "[detect] CustomProbe selected\n");
        std::unique_ptr<custom_stream_process> p(new custom_stream_process(conn, _app));
        return std::unique_ptr<base_data_process>(p.release());
    }
};

#endif
