#!/usr/bin/env bash
# HTTPS证书验证测试套件
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1 && pwd)"
cd "$SCRIPT_DIR"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 测试结果统计
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

print_header() {
    echo -e "\n${BLUE}$1${NC}"
    echo -e "${BLUE}$(printf '=%.0s' {1..60})${NC}"
}

print_test() {
    echo -e "\n${YELLOW}[测试 $((++TOTAL_TESTS))] $1${NC}"
}

check_result() {
    if [ $1 -eq 0 ]; then
        echo -e "${GREEN}✅ 通过${NC}"
        ((PASSED_TESTS++))
    else
        echo -e "${RED}❌ 失败${NC}"
        ((FAILED_TESTS++))
    fi
}

cleanup() {
    echo -e "\n${YELLOW}清理测试环境...${NC}"
    pkill -f https_server_validation || true
    pkill -f https_client_validation || true
    sleep 1
}

print_summary() {
    echo -e "\n${BLUE}$(printf '=%.0s' {1..60})${NC}"
    echo -e "${BLUE}📊 测试结果摘要${NC}"
    echo -e "${BLUE}$(printf '=%.0s' {1..60})${NC}"
    echo -e "总测试数: $TOTAL_TESTS"
    echo -e "${GREEN}通过: $PASSED_TESTS${NC}"
    echo -e "${RED}失败: $FAILED_TESTS${NC}"
    
    if [ $FAILED_TESTS -eq 0 ]; then
        echo -e "\n${GREEN}🎉 所有测试通过！${NC}"
        return 0
    else
        echo -e "\n${RED}⚠️  有测试失败，请检查上述输出${NC}"
        return 1
    fi
}

# 信号处理
trap cleanup EXIT
trap 'echo -e "\n${RED}测试被中断${NC}"; exit 130' INT

print_header "HTTPS证书验证测试框架"

# 检查依赖
print_test "检查测试依赖"
if ! command -v openssl >/dev/null 2>&1; then
    echo -e "${RED}❌ openssl 命令不可用${NC}"
    exit 1
fi

if [ ! -f ssl_cert_generator.sh ]; then
    echo -e "${RED}❌ 证书生成脚本不存在${NC}"
    exit 1
fi
check_result 0

# 生成测试证书
print_test "生成SSL测试证书"
./ssl_cert_generator.sh > /dev/null 2>&1
check_result $?

# 编译测试程序
print_test "编译HTTPS验证服务器"
g++ -std=c++17 -DENABLE_SSL -I../include -I../core \
    https_server_validation.cpp \
    ../lib/libmyframe.a ../lib/libsign.a \
    -lssl -lcrypto -lpthread \
    -o https_server_validation 2>/dev/null
check_result $?

print_test "编译HTTPS验证客户端"
g++ -std=c++17 https_client_validation.cpp \
    -lssl -lcrypto \
    -o https_client_validation 2>/dev/null
check_result $?

# 启动服务器
print_test "启动HTTPS验证服务器 (端口8443)"
./https_server_validation 8443 > server_validation.log 2>&1 &
SERVER_PID=$!
sleep 2

if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${RED}❌ 服务器启动失败${NC}"
    cat server_validation.log
    check_result 1
else
    check_result 0
fi

# 测试1: 基础HTTPS连接 (跳过证书验证)
print_test "基础HTTPS连接测试 (跳过证书验证)"
./https_client_validation --no-verify 127.0.0.1 8443 /health > test1.log 2>&1
check_result $?

# 测试2: 使用CA证书验证
print_test "使用CA证书的服务器验证"
./https_client_validation --ca ca.pem 127.0.0.1 8443 /cert/info > test2.log 2>&1
check_result $?

# 测试3: 证书验证详情
print_test "获取证书验证详细信息"
./https_client_validation --ca ca.pem 127.0.0.1 8443 /cert/verify > test3.log 2>&1
check_result $?

# 测试4: 错误证书验证 (应该失败)
print_test "错误证书验证测试 (期望失败)"
./https_client_validation --ca invalid.crt 127.0.0.1 8443 /health > test4.log 2>&1
if [ $? -ne 0 ]; then
    check_result 0  # 期望失败，所以成功
else
    check_result 1  # 不应该成功
fi

# 停止服务器
kill $SERVER_PID 2>/dev/null || true
sleep 1

# 测试5: 启动需要客户端证书的服务器
print_test "启动双向认证HTTPS服务器"
./https_server_validation --client-cert 8444 > server_mtls.log 2>&1 &
MTLS_SERVER_PID=$!
sleep 2

if ! kill -0 $MTLS_SERVER_PID 2>/dev/null; then
    echo -e "${RED}❌ mTLS服务器启动失败${NC}"
    cat server_mtls.log
    check_result 1
else
    check_result 0
    
    # 测试6: 双向认证 - 使用客户端证书
    print_test "双向认证测试 (使用客户端证书)"
    ./https_client_validation --ca ca.pem --cert client.crt --key client.key 127.0.0.1 8444 /cert/info > test6.log 2>&1
    check_result $?
    
    # 测试7: 双向认证 - 无客户端证书 (应该失败)
    print_test "双向认证失败测试 (无客户端证书)"
    ./https_client_validation --ca ca.pem 127.0.0.1 8444 /health > test7.log 2>&1
    if [ $? -ne 0 ]; then
        check_result 0  # 期望失败
    else
        check_result 1  # 不应该成功
    fi
    
    kill $MTLS_SERVER_PID 2>/dev/null || true
fi

# 测试8: 使用curl进行对比验证
print_test "curl对比验证测试"
if command -v curl >/dev/null 2>&1; then
    ./https_server_validation 8445 > server_curl.log 2>&1 &
    CURL_SERVER_PID=$!
    sleep 2
    
    curl -k -s https://127.0.0.1:8445/health > curl_test.log 2>&1
    if [ $? -eq 0 ] && grep -q "healthy" curl_test.log; then
        check_result 0
    else
        check_result 1
    fi
    
    kill $CURL_SERVER_PID 2>/dev/null || true
else
    echo -e "${YELLOW}curl 不可用，跳过对比测试${NC}"
    check_result 0
fi

cleanup
print_summary
