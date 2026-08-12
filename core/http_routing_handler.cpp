// http_routing_handler.cpp · Router 接入分发实现（Commit 4 / 子提交1）
#include "http_routing_handler.h"
#include "http_json.h"
#include <algorithm>
#include <set>

namespace myframe {

std::string RoutingHandler::join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ", "; s += v[i]; }
    return s;
}

std::vector<std::string> RoutingHandler::augment_allow(const std::vector<std::string>& allow) const {
    std::set<std::string> set(allow.begin(), allow.end());
    if (_auto_head && set.count("GET")) set.insert("HEAD");        // GET → 也支持 HEAD
    if (_auto_options) set.insert("OPTIONS");                      // auto-OPTIONS
    return std::vector<std::string>(set.begin(), set.end());      // set → 去重 + 字典序
}

void RoutingHandler::on_http_request(HttpContext& ctx) {
    const std::string& method = ctx.request().method;
    const std::string& url = ctx.request().url;

    MatchResult mr = _router.match(method, url);

    // 关键：所有路径都经过 MiddlewareChain（全局 + 统一 after），框架生成的错误/OPTIONS
    // 也不例外，否则 CORS/request-id/access-log 等全局 after 会在错误响应上缺失。
    //   - Found：全局(+未来路由级) middleware + 业务 handler；
    //   - 框架生成 400/404/405/OPTIONS：只执行“全局” middleware（route_mws=nullptr），
    //     用一个合成 handler 产出响应，不跑路由业务 handler / 路由级鉴权。

    if (mr.status == MatchStatus::Found) {
        ctx.set_route_params(mr.params);
        _chain.execute(ctx, mr.handler);
        return;
    }
    if (mr.status == MatchStatus::BadRequest) {                    // 非法百分号编码
        _chain.execute(ctx, [](HttpContext& c) { json_error(c.response(), 400, "bad_request"); });
        return;
    }
    if (mr.status == MatchStatus::NotFound) {
        // 404 文案沿用常见的 "not found"（与既有服务一致）；405/400 用 snake_case 错误码。
        _chain.execute(ctx, [](HttpContext& c) { json_error(c.response(), 404, "not found"); });
        return;
    }

    // MethodNotAllowed：显式 HEAD/OPTIONS 已在上面 Found 分支优先；这里做自动合成。
    if (method == "HEAD" && _auto_head) {
        MatchResult g = _router.match("GET", url);
        if (g.status == MatchStatus::Found) {
            ctx.set_route_params(g.params);
            ctx.set_suppress_body(true);                          // 复用 GET handler + 相同 middleware，仅发送层抑制 body
            _chain.execute(ctx, g.handler);
            return;
        }
    }
    if (method == "OPTIONS" && _auto_options) {
        std::string allow = join(augment_allow(mr.allow));
        // 合成 OPTIONS：只跑全局 middleware，不跑路由业务 handler / 路由级鉴权。
        _chain.execute(ctx, [allow](HttpContext& c) {
            c.response().status = 204;
            c.response().reason = "No Content";
            c.response().set_header("Allow", allow);
        });
        return;
    }
    // 普通 405（Allow 头 + JSON 体；均在合成 handler 内设置，经全局 middleware）
    std::string allow = join(augment_allow(mr.allow));
    _chain.execute(ctx, [allow](HttpContext& c) {
        c.response().reason = "Method Not Allowed";
        c.response().set_header("Allow", allow);
        json_error(c.response(), 405, "method_not_allowed");
    });
}

} // namespace myframe
