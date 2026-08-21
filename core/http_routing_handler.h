// http_routing_handler.h · Router 接入分发（Commit 4 / 子提交1）
// match → 注入 route_param → 组同一条 MiddlewareChain（全局 + 路由级）执行 → HEAD/OPTIONS 合成。
// 错误体经可配置 render_error 渲染（框架默认中性错误码；业务可覆盖文案，不污染其他用户）。
#ifndef __MYFRAME_HTTP_ROUTING_HANDLER_H__
#define __MYFRAME_HTTP_ROUTING_HANDLER_H__

#include "protocol_context.h"
#include "http_router.h"
#include "http_middleware.h"
#include <functional>
#include <string>
#include <vector>

namespace myframe {

struct RoutingOptions {
    bool auto_head = true;
    bool auto_options = true;
    // 错误体渲染：(ctx, http_status, framework_code) → 写入响应。
    // 空则用默认 json_error(status, code)（code 为 "not_found"/"method_not_allowed"/"bad_request"）。
    // 业务可覆盖（如把 "not_found" 渲染成旧文案），不影响框架其他用户。
    std::function<void(HttpContext&, int, const std::string&)> render_error;
};

class RoutingHandler : public IProtocolHandler {
public:
    RoutingHandler(const Router& router, const MiddlewareChain& chain,
                   RoutingOptions options = RoutingOptions())
        : _router(router), _chain(chain), _opt(std::move(options)) {}

    // 兼容旧构造：仅指定 auto_head/auto_options。
    RoutingHandler(const Router& router, const MiddlewareChain& chain,
                   bool auto_head, bool auto_options)
        : _router(router), _chain(chain) { _opt.auto_head = auto_head; _opt.auto_options = auto_options; }

    void on_http_request(HttpContext& ctx) override;

private:
    std::vector<std::string> augment_allow(const std::vector<std::string>& allow) const;
    static std::string join(const std::vector<std::string>& v);
    void render_error(HttpContext& ctx, int status, const std::string& code) const;

    const Router& _router;
    const MiddlewareChain& _chain;
    RoutingOptions _opt;
};

} // namespace myframe

#endif
