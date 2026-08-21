// http_json.h · 响应侧 JSON 便捷（提交3，最小实现）
// 约束（docs/http-router-rfc.md §5）：自动 Content-Type；body 由 HttpResponse 值持有；
// 公共接口不引入 RapidJSON/JWT（仅接受 std::string）；序列化失败由业务走统一 500 路径。
// 这里只做“已序列化字符串 → 响应”和标准错误体，不含解析（解析属业务层，避免把 JSON 库带进公共接口）。
#ifndef __MYFRAME_HTTP_JSON_H__
#define __MYFRAME_HTTP_JSON_H__

#include "app_handler_v2.h"
#include <string>

namespace myframe {

// 最小 JSON 字符串转义（用于框架自造的错误体，不做通用序列化）。
// 规则：转义 " \ 及所有控制字符(<0x20，命名转义或 \u00XX)。
// **非法 UTF-8 规则（明确）**：本函数假定入参为 UTF-8；字节 >=0x80 **原样透传、不校验**。
// 因此非 UTF-8 入参会产出非 UTF-8 输出——调用方须保证消息为 UTF-8（框架错误体均为 ASCII，安全）。
inline std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            default:
                if (c < 0x20) {                      // 其它控制字符 → \u00XX
                    static const char* hex = "0123456789abcdef";
                    o += "\\u00";
                    o += hex[(c >> 4) & 0xF];
                    o += hex[c & 0xF];
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

// 标准错误响应：{"error":"<msg>"} + 正确 Content-Type + 状态码。body 由 response 值持有。
inline void json_error(HttpResponse& res, int code, const std::string& msg) {
    res.status = code;
    res.set_json(std::string("{\"error\":\"") + json_escape(msg) + "\"}");
}

} // namespace myframe

#endif
