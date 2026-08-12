// http_router.h · HTTP 路由表（提交2）
// 纯路由结构，不依赖运行中的服务，可独立单测。契约见 docs/http-router-rfc.md §3。
//   - method + path 注册；静态段优先于 {param}（带回溯，避免静态前缀走进死路）；
//   - 整段 {param} 捕获；分段后再按段 URL decode（%2F 不改变层级）；非法百分号编码 → 400；
//   - query 不参与匹配（匹配前剥离 ?...）；尾斜杠规范化（/x 等价 /x/）；
//   - 404 / 405(+Allow 去重且有序)；注册期冲突检测（重复路由、同位不同名 {param}）；
//   - freeze() 后只读：结构不再改变，match() 无写、并发只读安全（无锁）。
#ifndef __MYFRAME_HTTP_ROUTER_H__
#define __MYFRAME_HTTP_ROUTER_H__

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace myframe {

class HttpContext;   // 前置声明：本提交不绑定 Context（route_param/分发留待后续提交）
typedef std::function<void(HttpContext&)> RouteHandler;

enum class MatchStatus { Found, NotFound, MethodNotAllowed, BadRequest };

struct MatchResult {
    MatchStatus status = MatchStatus::NotFound;
    RouteHandler handler;                          // Found 时有效
    std::map<std::string, std::string> params;     // 捕获的 {param}（已 URL decode）
    std::vector<std::string> allow;                // MethodNotAllowed 时的 Allow（去重、字典序）
};

class Router {
public:
    Router();

    // 注册（freeze 前）。冲突或非法模板 → 抛 std::runtime_error（启动期失败，不静默覆盖）。
    Router& add(const std::string& method, const std::string& path, RouteHandler h);
    Router& get(const std::string& path, RouteHandler h)  { return add("GET", path, std::move(h)); }
    Router& post(const std::string& path, RouteHandler h) { return add("POST", path, std::move(h)); }
    Router& put(const std::string& path, RouteHandler h)  { return add("PUT", path, std::move(h)); }
    Router& del(const std::string& path, RouteHandler h)  { return add("DELETE", path, std::move(h)); }

    // 冻结：之后 add() 抛异常；match() 变为纯只读、可并发。
    void freeze();
    bool frozen() const { return _frozen; }

    // 匹配。method 大小写敏感（HTTP 方法约定全大写）；raw_path 可含 query。
    MatchResult match(const std::string& method, const std::string& raw_path) const;

private:
    struct Node {
        std::map<std::string, std::unique_ptr<Node> > static_children;  // 静态段（map 保证稳定序）
        std::unique_ptr<Node> param_child;                              // 唯一参数段
        std::string param_name;                                         // 参数段名（冲突检测 + 捕获）
        std::map<std::string, RouteHandler> methods;                    // 终结点：method → handler
    };

    // 模板分段：规范化尾斜杠 + 按 '/' 切分。seg 以 {name} 形式即参数段。
    static std::vector<std::string> split_template(const std::string& path);
    // 请求分段：剥 query + 规范化尾斜杠 + 切分（不 decode，decode 在匹配时按段做）。
    static std::vector<std::string> split_request(const std::string& raw_path);
    // 按段百分号解码；非法编码返回 false。
    static bool percent_decode(const std::string& in, std::string& out);
    // DFS：返回命中的终结节点（methods 非空），静态优先 + 回溯；失败返回 nullptr。
    const Node* match_node(const Node* node, const std::vector<std::string>& segs,
                           size_t idx, std::map<std::string, std::string>& params) const;

    Node _root;
    bool _frozen;
};

} // namespace myframe

#endif
