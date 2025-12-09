#pragma once

#include <string>
#include <map>
#include <memory>
#include <functional>

#include "common_obj_container.h"
#include "app_handler_v2.h"
#include "base_net_obj.h"

struct ClientBuildCtx {
    common_obj_container* container{nullptr};
    myframe::IApplicationHandler* handler{nullptr};
    std::map<std::string, std::string> headers; // for HTTP
    std::string method{"GET"};
    std::string body;
};

using ClientBuilder = std::function<std::shared_ptr<base_net_obj>(const std::string& url, const ClientBuildCtx&)>;

class ClientConnRouter {
public:
    void register_factory(const std::string& scheme, ClientBuilder b) { _map[scheme] = std::move(b); }

    std::shared_ptr<base_net_obj> create_and_enqueue(const std::string& url, const ClientBuildCtx& ctx) {
        std::string s = parse_scheme(url);
        auto it = _map.find(s);
        if (it == _map.end()) return nullptr;
        std::shared_ptr<base_net_obj> net = it->second(url, ctx);
        if (net && ctx.container) {
            ctx.container->push_real_net(net);
        }
        return net;
    }

    static std::string parse_scheme(const std::string& url) {
        auto p = url.find("://");
        if (p == std::string::npos) return std::string();
        std::string s = url.substr(0, p);
        for (auto& c : s) c = (char)tolower(c);
        return s;
    }

private:
    std::map<std::string, ClientBuilder> _map;
};

// Register default builders for http/https/ws/wss
void register_default_client_builders(ClientConnRouter& r);

