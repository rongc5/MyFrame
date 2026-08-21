// http_router.cpp · HTTP 路由表实现（提交2）
#include "http_router.h"
#include <set>
#include <stdexcept>

namespace myframe {

Router::Router() : _frozen(false) {}

// 冻结后禁止再挂载路由级 middleware（防止已保存的 Route 代理绕过 freeze 改动内部 vector）。
Router::Route& Router::Route::use(Middleware mw) {
    if (owner_ && owner_->_frozen)
        throw std::runtime_error("Router::Route::use() after freeze()");
    if (entry_) entry_->mws.push_back(std::move(mw));
    return *this;
}

// ---- 分段辅助 ----

// 去掉全部尾部 '/'（根 "/" 归一为空段序列）。
static std::string strip_trailing_slashes(const std::string& p) {
    size_t end = p.size();
    while (end > 1 && p[end - 1] == '/') --end;   // 保留至少 1 个字符以区分根
    std::string s = p.substr(0, end);
    if (s == "/") return std::string();           // 根 → 空
    return s;
}

// 按 '/' 切分（路径以 '/' 开头，忽略首个空段）。
static std::vector<std::string> split_slash(const std::string& path) {
    std::vector<std::string> out;
    std::string norm = strip_trailing_slashes(path);
    if (norm.empty()) return out;                 // 根
    size_t i = 0;
    if (!norm.empty() && norm[0] == '/') i = 1;   // 跳过开头 '/'
    std::string cur;
    for (; i < norm.size(); ++i) {
        if (norm[i] == '/') { out.push_back(cur); cur.clear(); }
        else cur.push_back(norm[i]);
    }
    out.push_back(cur);
    return out;
}

std::vector<std::string> Router::split_template(const std::string& path) {
    return split_slash(path);
}

std::vector<std::string> Router::split_request(const std::string& raw_path) {
    std::string p = raw_path;
    size_t q = p.find('?');                        // 剥离 query，不参与匹配
    if (q != std::string::npos) p = p.substr(0, q);
    return split_slash(p);
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool Router::percent_decode(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= in.size()) return false;                 // 截断的 %xx
            int hi = hex_val(in[i + 1]), lo = hex_val(in[i + 2]);
            if (hi < 0 || lo < 0) return false;                   // 非法十六进制
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(c);   // 路径中 '+' 保持字面（不转空格，那是 query 语义）
        }
    }
    return true;
}

// ---- 注册 ----

Router::Route Router::add(const std::string& method, const std::string& path, RouteHandler h) {
    if (_frozen) throw std::runtime_error("Router::add after freeze(): " + method + " " + path);

    std::vector<std::string> segs = split_template(path);
    std::set<std::string> seen_params;   // 同一路径内参数名必须唯一（否则回溯 erase 会误删同名捕获）
    Node* node = &_root;
    for (size_t i = 0; i < segs.size(); ++i) {
        const std::string& seg = segs[i];
        bool is_param = seg.size() >= 2 && seg.front() == '{' && seg.back() == '}';
        if (is_param) {
            std::string name = seg.substr(1, seg.size() - 2);
            if (name.empty() || name.find('{') != std::string::npos ||
                name.find('}') != std::string::npos)
                throw std::runtime_error("Router: invalid param segment '" + seg + "' in " + path);
            if (!seen_params.insert(name).second)
                throw std::runtime_error("Router: duplicate param name '{" + name + "}' in " + path);
            if (node->param_child) {
                // 同位置已有参数段：名字不同视为结构冲突（{id} 与 {name}）
                if (node->param_name != name)
                    throw std::runtime_error(
                        "Router: conflicting param names '{" + node->param_name + "}' vs '{" +
                        name + "}' at same position in " + path);
            } else {
                node->param_child.reset(new Node());
                node->param_name = name;
            }
            node = node->param_child.get();
        } else {
            if (seg.empty())
                throw std::runtime_error("Router: empty path segment (e.g. '//') in " + path);
            if (seg.find('{') != std::string::npos || seg.find('}') != std::string::npos)
                throw std::runtime_error("Router: '{' '}' only allowed as whole segment: '" + seg + "'");
            std::unique_ptr<Node>& child = node->static_children[seg];
            if (!child) child.reset(new Node());
            node = child.get();
        }
    }
    if (node->methods.find(method) != node->methods.end())
        throw std::runtime_error("Router: duplicate route " + method + " " + path);
    Entry& e = node->methods[method];
    e.handler = std::move(h);
    return Route(this, &e);
}

void Router::freeze() { _frozen = true; }

// ---- 匹配 ----

const Router::Node* Router::match_node(const Node* node, const std::vector<std::string>& segs,
                                       size_t idx, std::map<std::string, std::string>& params) const {
    if (idx == segs.size())
        return node->methods.empty() ? nullptr : node;   // 只有带方法的终结点算命中

    const std::string& seg = segs[idx];

    // 静态优先
    std::map<std::string, std::unique_ptr<Node> >::const_iterator it = node->static_children.find(seg);
    if (it != node->static_children.end()) {
        const Node* r = match_node(it->second.get(), segs, idx + 1, params);
        if (r) return r;                              // 静态子树命中即用（静态优先）
    }
    // 回溯到参数段
    if (node->param_child) {
        params[node->param_name] = seg;               // 捕获（已 decode）
        const Node* r = match_node(node->param_child.get(), segs, idx + 1, params);
        if (r) return r;
        params.erase(node->param_name);               // 未命中则撤销捕获
    }
    return nullptr;
}

MatchResult Router::match(const std::string& method, const std::string& raw_path) const {
    MatchResult res;

    std::vector<std::string> raw_segs = split_request(raw_path);
    // 分段之后再逐段解码：%2F 保持段内字面，不改变路由层级
    std::vector<std::string> segs;
    segs.reserve(raw_segs.size());
    for (size_t i = 0; i < raw_segs.size(); ++i) {
        std::string dec;
        if (!percent_decode(raw_segs[i], dec)) {      // 非法百分号编码 → 400
            res.status = MatchStatus::BadRequest;
            return res;
        }
        segs.push_back(dec);
    }

    std::map<std::string, std::string> params;
    const Node* term = match_node(&_root, segs, 0, params);
    if (!term) { res.status = MatchStatus::NotFound; return res; }

    std::map<std::string, Entry>::const_iterator m = term->methods.find(method);
    if (m != term->methods.end()) {
        res.status = MatchStatus::Found;
        res.handler = m->second.handler;
        res.route_mws = &m->second.mws;          // 指向 Router 内部；freeze 后只读稳定
        res.params.swap(params);
        return res;
    }
    // 路径匹配但方法不符 → 405 + Allow（map 键天然去重且字典序稳定）
    res.status = MatchStatus::MethodNotAllowed;
    for (std::map<std::string, Entry>::const_iterator k = term->methods.begin();
         k != term->methods.end(); ++k)
        res.allow.push_back(k->first);
    return res;
}

} // namespace myframe
