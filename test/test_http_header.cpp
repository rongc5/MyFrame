// test_http_header.cpp · 回归测试
//   1) http_res_process::parse_header 去掉 header 值冒号后的 OWS（RFC7230）
//   2) myframe::HttpRequest::get_header 大小写不敏感
// 对应修复：fix(http): normalize header OWS and support case-insensitive lookup
#include "http_res_process.h"
#include "app_handler_v2.h"
#include "common_def.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); ++g_fail; } } while (0)

// 测试子类：parse_header 为 protected，用 using 暴露以便单测
struct TestResProc : public http_res_process {
    TestResProc() : http_res_process(nullptr) {}
    using http_res_process::parse_header;
};

int main() {
    // 1) OWS：值前后空格应被去掉，避免消费方 rfind("Bearer ",0) 失配
    {
        TestResProc proc;                 // 构造不解引用连接指针，可空构造
        std::string block =
            "GET /x HTTP/1.1\r\n"
            "Authorization: Bearer XYZ\r\n"
            "X-Trim:   spaced   \r\n";
        proc.parse_header(block);
        http_req_head_para& hp = proc.get_req_head_para();
        std::string* a = hp.get_header("Authorization");
        CHECK(a != NULL && *a == "Bearer XYZ");
        std::string* t = hp.get_header("X-Trim");
        CHECK(t != NULL && *t == "spaced");
    }

    // 2) HttpRequest::get_header 大小写不敏感
    {
        myframe::HttpRequest req;
        req.headers["Authorization"] = "Bearer T";
        CHECK(req.get_header("Authorization") == "Bearer T");   // 精确
        CHECK(req.get_header("authorization") == "Bearer T");   // 全小写
        CHECK(req.get_header("AUTHORIZATION") == "Bearer T");   // 全大写
        CHECK(req.get_header("Nope").empty());                  // 不存在
    }

    if (g_fail == 0) { printf("test_http_header: ALL PASS\n"); return 0; }
    printf("test_http_header: %d FAIL\n", g_fail);
    return 1;
}
