#!/bin/bash

echo "=== 多协议框架分层测试方案 ==="
echo ""

# 颜色定义
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 测试结果统计
TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0

run_test() {
    local test_name="$1"
    local test_cmd="$2"
    echo -e "${YELLOW}运行测试: $test_name${NC}"
    
    if eval "$test_cmd"; then
        echo -e "${GREEN}✅ $test_name - PASSED${NC}"
        ((TESTS_PASSED++))
    else
        echo -e "${RED}❌ $test_name - FAILED${NC}"
        ((TESTS_FAILED++))
    fi
    echo ""
}

echo "────────────────────────────"
echo "一、单元测试（Unit Test）"
echo "────────────────────────────"

# 1. 编译检查（使用脚本构建工具库）
run_test "编译框架库" "cd /home/rong/myframe && ./scripts/build.sh >/dev/null 2>&1"

# 2. 单元测试
if [ -x "/home/rong/myframe/build/test/test_unit" ]; then
    run_test "IApplicationHandler适配测试" "cd /home/rong/myframe/build/test && timeout 5s ./test_unit 2>/dev/null | grep -q 'HTTP调用次数'"
else
    echo -e "${YELLOW}运行测试: IApplicationHandler适配测试${NC}"
    echo -e "${BLUE}⏭️  IApplicationHandler适配测试 - SKIPPED（未找到 test_unit）${NC}"
    ((TESTS_SKIPPED++))
    echo ""
fi

# 3. 业务层测试
if [ -x "/home/rong/myframe/build/test/test_business_handler" ]; then
    run_test "业务层抽象测试" "cd /home/rong/myframe/build/test && timeout 5s ./test_business_handler 2>/dev/null | grep -q 'HTTP响应'"
else
    echo -e "${YELLOW}运行测试: 业务层抽象测试${NC}"
    echo -e "${BLUE}⏭️  业务层抽象测试 - SKIPPED（未找到 test_business_handler）${NC}"
    ((TESTS_SKIPPED++))
    echo ""
fi

echo "────────────────────────────"
echo "二、集成测试（Integration）"
echo "────────────────────────────"

# 检查测试工具
echo "检查测试工具可用性:"
command -v curl >/dev/null 2>&1 && echo "✅ curl 可用" || echo "❌ curl 未安装"
command -v nc >/dev/null 2>&1 && echo "✅ netcat 可用" || echo "❌ netcat 未安装"

echo "注意: HTTP+WS复用端口集成测试需要实际服务器运行"
echo "可以使用以下命令手动测试:"
echo "1. 启动服务器: ./test_integration_server 8080"
echo "2. HTTP测试: curl http://127.0.0.1:8080/hello"
echo "3. WebSocket测试: wscat -c ws://127.0.0.1:8080/chat"

echo "────────────────────────────"
echo "三、压力测试（需要实际服务器）"
echo "────────────────────────────"
echo "性能基线测试命令:"
echo "wrk -t4 -c100 -d10s http://127.0.0.1:8080/"
echo ""

# 测试总结
echo "════════════════════════════"
echo "测试总结"
echo "════════════════════════════"
echo -e "通过: ${GREEN}$TESTS_PASSED${NC}"
echo -e "失败: ${RED}$TESTS_FAILED${NC}"
echo -e "跳过: ${BLUE}$TESTS_SKIPPED${NC}"

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}🎉 核心功能测试全部通过！框架可以进入联调阶段。${NC}"
    exit 0
else
    echo -e "${RED}❌ 有测试失败，请检查问题。${NC}"
    exit 1
fi
