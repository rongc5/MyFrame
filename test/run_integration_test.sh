#!/bin/bash

# 多协议框架集成测试脚本
PORT=${1:-8080}
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "=== 多协议框架集成测试 ==="
echo "端口: $PORT"
echo ""

# 编译测试程序
echo "编译测试程序..."
cd /home/rong/myframe/test

g++ -g -Wall -I../include/ -DDEBUG=1 -std=c++11 -o test_real_server test_real_server.cpp -L../lib -lmyframe -lpthread -lsign -lssl -lcrypto
g++ -g -Wall -I../include/ -DDEBUG=1 -std=c++11 -o simple_http_client simple_http_client.cpp
g++ -g -Wall -I../include/ -DDEBUG=1 -std=c++11 -o simple_ws_client simple_ws_client.cpp

if [ $? -ne 0 ]; then
    echo -e "${RED}❌ 编译失败${NC}"
    exit 1
fi

echo -e "${GREEN}✅ 编译成功${NC}"

# 启动服务器
echo "启动多协议服务器..."
./test_real_server $PORT &
SERVER_PID=$!

# 等待服务器启动
sleep 3

# 检查服务器是否启动成功
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${RED}❌ 服务器启动失败${NC}"
    exit 1
fi

echo -e "${GREEN}✅ 服务器启动成功 (PID: $SERVER_PID)${NC}"

# 测试函数
run_test() {
    local test_name="$1"
    local test_cmd="$2"
    echo -e "${YELLOW}运行: $test_name${NC}"
    
    if eval "$test_cmd" >/dev/null 2>&1; then
        echo -e "${GREEN}✅ $test_name${NC}"
        return 0
    else
        echo -e "${RED}❌ $test_name${NC}"
        return 1
    fi
}

TESTS_PASSED=0
TESTS_FAILED=0

# HTTP测试
echo ""
echo "开始HTTP协议测试..."
if run_test "HTTP客户端测试" "./simple_http_client $PORT"; then
    ((TESTS_PASSED++))
else
    ((TESTS_FAILED++))
fi

if run_test "curl /hello测试" "curl -s http://127.0.0.1:$PORT/hello | grep -q 'Hello from Real'"; then
    ((TESTS_PASSED++))
else
    ((TESTS_FAILED++))
fi

if run_test "curl /api/status测试" "curl -s http://127.0.0.1:$PORT/api/status | grep -q 'protocol.*http'"; then
    ((TESTS_PASSED++))
else
    ((TESTS_FAILED++))
fi

# WebSocket测试
echo ""
echo "开始WebSocket协议测试..."
if run_test "WebSocket客户端测试" "./simple_ws_client $PORT"; then
    ((TESTS_PASSED++))
else
    ((TESTS_FAILED++))
fi

# 如果有wscat，进行更详细的WebSocket测试
if command -v wscat >/dev/null 2>&1; then
    echo "进行WebSocket交互测试..."
    timeout 5s wscat -c ws://127.0.0.1:$PORT/chat -x 'ping' 2>/dev/null | grep -q pong
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✅ WebSocket ping/pong测试${NC}"
        ((TESTS_PASSED++))
    else
        echo -e "${RED}❌ WebSocket ping/pong测试${NC}"
        ((TESTS_FAILED++))
    fi
else
    echo "跳过WebSocket交互测试 (wscat未安装)"
fi

# 负载测试
echo ""
echo "开始简单负载测试..."
if command -v ab >/dev/null 2>&1; then
    if run_test "HTTP负载测试 (100请求)" "ab -n 100 -c 5 -q http://127.0.0.1:$PORT/hello"; then
        ((TESTS_PASSED++))
    else
        ((TESTS_FAILED++))
    fi
else
    echo "跳过负载测试 (apache bench未安装)"
fi

# 关闭服务器
echo ""
echo "关闭服务器..."
kill -TERM $SERVER_PID 2>/dev/null
sleep 2

# 强制关闭（如果还在运行）
if kill -0 $SERVER_PID 2>/dev/null; then
    kill -KILL $SERVER_PID 2>/dev/null
fi

# 测试结果
echo ""
echo "════════════════════════════"
echo "集成测试结果"
echo "════════════════════════════"
echo -e "通过: ${GREEN}$TESTS_PASSED${NC}"
echo -e "失败: ${RED}$TESTS_FAILED${NC}"

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}🎉 集成测试全部通过！${NC}"
    echo "多协议框架HTTP/WebSocket同端口功能正常"
    exit 0
else
    echo -e "${RED}❌ 有测试失败，请检查服务器日志${NC}"
    exit 1
fi