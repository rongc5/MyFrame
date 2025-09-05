#pragma once

#include <string>
#include <map>
#include <memory>

struct HttpResult {
    int status{0};
    std::map<std::string,std::string> headers;
    std::string body;
};

struct IRequest {
    virtual ~IRequest() {}
    virtual HttpResult get(const std::string& url, const std::map<std::string,std::string>& headers = {}) = 0;
};

struct IRequestFactory {
    virtual ~IRequestFactory() {}
    virtual std::unique_ptr<IRequest> create() = 0;
};

// Router: pick factory by scheme (e.g. "http", "https")
class RequestRouter {
public:
    void register_factory(const std::string& scheme, std::shared_ptr<IRequestFactory> f) {
        _map[scheme] = f;
    }
    HttpResult get(const std::string& url, const std::map<std::string,std::string>& headers = {}) {
        std::string scheme = parse_scheme(url);
        auto it = _map.find(scheme);
        if (it == _map.end() || !it->second) return HttpResult{};
        std::unique_ptr<IRequest> cli = it->second->create();
        return cli ? cli->get(url, headers) : HttpResult{};
    }
private:
    static std::string parse_scheme(const std::string& url) {
        auto p = url.find("://");
        if (p == std::string::npos) return std::string();
        std::string s = url.substr(0, p);
        for (auto& c : s) c = (char)tolower(c);
        return s;
    }
    std::map<std::string,std::shared_ptr<IRequestFactory>> _map;
};

// Factory helpers (defined in http1_client.cpp)
std::shared_ptr<IRequestFactory> make_http1_factory();
std::shared_ptr<IRequestFactory> make_ws_factory();
