// test_http_json.cpp · 响应侧 JSON 便捷回归（提交3）
#include "http_json.h"
#include <cstdio>
#include <string>

using namespace myframe;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    // 转义：引号/反斜杠/换行/控制字符
    CHECK(json_escape("a\"b\\c") == "a\\\"b\\\\c");
    CHECK(json_escape("x\ny") == "x\\ny");
    CHECK(json_escape(std::string("\x01")) == "\\u0001");
    // 非法 UTF-8 规则：字节 >=0x80 原样透传（不校验）——已在 http_json.h 明确
    CHECK(json_escape(std::string("\xC3\x28")) == std::string("\xC3\x28"));

    // json_error：状态码 + 标准错误体 + Content-Type
    HttpResponse r;
    json_error(r, 401, "unauthorized");
    CHECK(r.status == 401);
    CHECK(r.body == "{\"error\":\"unauthorized\"}");
    CHECK(r.headers.count("Content-Type") == 1);
    CHECK(r.headers["Content-Type"].find("application/json") != std::string::npos);

    // 消息含引号 → 正确转义，body 仍是合法 JSON 片段
    HttpResponse r2;
    json_error(r2, 400, "bad \"x\"");
    CHECK(r2.body == "{\"error\":\"bad \\\"x\\\"\"}");

    if (g_fail == 0) { printf("test_http_json: ALL PASS\n"); return 0; }
    printf("test_http_json: %d FAIL\n", g_fail);
    return 1;
}
