// http_routing_handler.cpp · Router 接入分发实现（Commit 4 / 子提交1）
#include "http_routing_handler.h"
#include "http_json.h"
#include <set>

namespace myframe {

std::string RoutingHandler::join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ", "; s += v[i]; }
    return s;
}

std::vector<std::string> RoutingHandler::augment_allow(const std::vector<std::string>& allow) const {
    std::set<std::string> set(allow.begin(), allow.end());
    if (_opt.auto_head && set.count("GET")) set.insert("HEAD");
    if (_opt.auto_options) set.insert("OPTIONS");
    return std::vector<std::string>(set.begin(), set.end());   // set → 去重 + 字典序
}

void RoutingHandler::render_error(HttpContext& ctx, int status, const std::string& code) const {
    if (_opt.render_error) _opt.render_error(ctx, status, code);
    else json_error(ctx.response(), status, code);             // 框架默认：{"error":"<code>"}
}

void RoutingHandler::on_http_request(HttpContext& ctx) {
    const std::string& method = ctx.request().method;
    const std::string& url = ctx.request().url;

    MatchResult mr = _router.match(method, url);

    // 所有路径都经 MiddlewareChain（全局 + 统一 after）。框架生成 400/404/405/OPTIONS
    // 只跑全局 middleware（route_mws=nullptr，不跑路由业务 handler / 路由级鉴权）。
    if (mr.status == MatchStatus::Found) {
        ctx.set_route_params(mr.params);
        _chain.execute(ctx, mr.handler, mr.route_mws);   // 全局 + 路由级 middleware
        return;
    }
    if (mr.status == MatchStatus::BadRequest) {
        _chain.execute(ctx, [this](HttpContext& c) { render_error(c, 400, "bad_request"); });
        return;
    }
    if (mr.status == MatchStatus::NotFound) {
        _chain.execute(ctx, [this](HttpContext& c) { render_error(c, 404, "not_found"); });
        return;
    }

    // MethodNotAllowed：显式 HEAD/OPTIONS 已在 Found 优先；这里自动合成。
    if (method == "HEAD" && _opt.auto_head) {
        MatchResult g = _router.match("GET", url);
        if (g.status == MatchStatus::Found) {
            ctx.set_route_params(g.params);
            ctx.set_suppress_body(true);                 // 复用 GET handler + 相同 middleware，仅发送层抑制 body
            _chain.execute(ctx, g.handler, g.route_mws);
            return;
        }
    }
    if (method == "OPTIONS" && _opt.auto_options) {
        std::string allow = join(augment_allow(mr.allow));
        _chain.execute(ctx, [allow](HttpContext& c) {
            c.response().status = 204;
            c.response().reason = "No Content";
            c.response().set_header("Allow", allow);
        });
        return;
    }
    // 普通 405（Allow 头 + 错误体，经全局 middleware）
    std::string allow = join(augment_allow(mr.allow));
    _chain.execute(ctx, [allow, this](HttpContext& c) {
        c.response().reason = "Method Not Allowed";
        c.response().set_header("Allow", allow);
        render_error(c, 405, "method_not_allowed");
    });
}

} // namespace myframe
