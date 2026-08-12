// http_router.h · HTTP 路由表（提交2 + 路由级 middleware）
// 契约见 docs/http-router-rfc.md §3。
//   - method + path 注册；静态段优先于 {param}（带回溯）；整段 {param} 捕获；
//   - 分段后再按段 URL decode（%2F 不改变层级）；非法百分号编码 → 400；
//   - query 不参与匹配；尾斜杠规范化；404 / 405(+Allow 去重且有序)；注册期冲突检测；
//   - freeze() 后只读、并发安全无锁；
//   - 路由级 middleware：router.get(path, h).use(mw)；匹配结果携带该路由的 middleware 列表，
//     由分发层与全局 middleware 组成同一条 MiddlewareChain（短路/异常/异步统一 finalize）。
#ifndef __MYFRAME_HTTP_ROUTER_H__
#define __MYFRAME_HTTP_ROUTER_H__

#include "protocol_context.h"     // HttpContext / Middleware / BeforeMiddleware / AfterMiddleware
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace myframe {

typedef std::function<void(HttpContext&)> RouteHandler;

enum class MatchStatus { Found, NotFound, MethodNotAllowed, BadRequest };

struct MatchResult {
    MatchStatus status = MatchStatus::NotFound;
    RouteHandler handler;                          // Found 时有效
    std::map<std::string, std::string> params;     // 捕获的 {param}（已 URL decode）
    std::vector<std::string> allow;                // MethodNotAllowed 时的 Allow（去重、字典序）
    const std::vector<Middleware>* route_mws = 0;  // Found 时指向该路由的中间件（指向 Router 内部，freeze 后只读稳定）
};

class Router {
public:
    Router();

    // 单条路由（method + handler + 其路由级 middleware）。
    struct Entry {
        RouteHandler handler;
        std::vector<Middleware> mws;   // 路由级 middleware（进入顺序）
    };

    // 注册返回的轻量代理：链式挂载路由级 middleware。
    // 持有所属 Router*，freeze 后 .use()/.use_before()/.use_after() 抛异常（禁止冻结后改动）。
    class Route {
    public:
        Route& use(Middleware mw);   // 定义在 .cpp（此处 Router 尚不完整）
        Route& use_before(BeforeMiddleware b) { return use(Middleware(std::move(b), AfterMiddleware())); }
        Route& use_after(AfterMiddleware a)   { return use(Middleware(BeforeMiddleware(), std::move(a))); }
    private:
        friend class Router;
        Route(Router* owner, Entry* e) : owner_(owner), entry_(e) {}
        Router* owner_;
        Entry* entry_;
    };

    // 注册（freeze 前）。冲突或非法模板 → 抛 std::runtime_error（启动期失败）。
    Route add(const std::string& method, const std::string& path, RouteHandler h);
    Route get(const std::string& path, RouteHandler h)  { return add("GET", path, std::move(h)); }
    Route post(const std::string& path, RouteHandler h) { return add("POST", path, std::move(h)); }
    Route put(const std::string& path, RouteHandler h)  { return add("PUT", path, std::move(h)); }
    Route del(const std::string& path, RouteHandler h)  { return add("DELETE", path, std::move(h)); }

    void freeze();
    bool frozen() const { return _frozen; }

    MatchResult match(const std::string& method, const std::string& raw_path) const;

private:
    struct Node {
        std::map<std::string, std::unique_ptr<Node> > static_children;
        std::unique_ptr<Node> param_child;
        std::string param_name;
        std::map<std::string, Entry> methods;   // method → Entry（handler + 路由级 mws）
    };

    static std::vector<std::string> split_template(const std::string& path);
    static std::vector<std::string> split_request(const std::string& raw_path);
    static bool percent_decode(const std::string& in, std::string& out);
    const Node* match_node(const Node* node, const std::vector<std::string>& segs,
                           size_t idx, std::map<std::string, std::string>& params) const;

    Node _root;
    bool _frozen;
};

} // namespace myframe

#endif
