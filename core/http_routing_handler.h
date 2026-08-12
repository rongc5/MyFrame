// http_routing_handler.h · Router 接入分发（Commit 4 / 子提交1）
// 把 Router + MiddlewareChain 接到 Level 2 的 on_http_request：
//   match → 注入 route_param → chain.execute(handler)；并按 §3 策略合成 HEAD/OPTIONS。
// HEAD body 抑制放在发送层（ctx.set_suppress_body），不污染纯 Router。
#ifndef __MYFRAME_HTTP_ROUTING_HANDLER_H__
#define __MYFRAME_HTTP_ROUTING_HANDLER_H__

#include "protocol_context.h"
#include "http_router.h"
#include "http_middleware.h"
#include <string>
#include <vector>

namespace myframe {

class RoutingHandler : public IProtocolHandler {
public:
    // router / chain 由调用方持有（生命周期须覆盖本 handler）。
    RoutingHandler(const Router& router, const MiddlewareChain& chain,
                   bool auto_head = true, bool auto_options = true)
        : _router(router), _chain(chain), _auto_head(auto_head), _auto_options(auto_options) {}

    void on_http_request(HttpContext& ctx) override;

private:
    // 组装 Allow：GET 存在则补 HEAD；auto_options 则补 OPTIONS；去重 + 字典序。
    std::vector<std::string> augment_allow(const std::vector<std::string>& allow) const;
    static std::string join(const std::vector<std::string>& v);

    const Router& _router;
    const MiddlewareChain& _chain;
    bool _auto_head;
    bool _auto_options;
};

} // namespace myframe

#endif
