#!/bin/bash
# MyFrame 示例程序快速测试脚本
# 用途：快速编译并测试所有示例是否可运行

set -e  # 遇到错误立即退出

echo "========================================"
echo "  MyFrame 示例程序测试脚本"
echo "========================================"
echo ""

# 颜色定义
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 获取脚本所在目录
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"

echo "项目根目录: $PROJECT_ROOT"
echo "编译目录: $BUILD_DIR"
echo ""

# 1. 编译所有示例
echo "========================================"
echo "步骤 1: 编译所有示例"
echo "========================================"

if [ ! -d "$BUILD_DIR" ]; then
    echo "创建编译目录..."
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

echo "运行 CMake..."
cmake .. || {
    echo -e "${RED}? CMake 配置失败${NC}"
    exit 1
}

echo ""
echo "编译示例程序..."
make -j$(nproc) || {
    echo -e "${RED}? 编译失败${NC}"
    exit 1
}

echo -e "${GREEN}? 所有示例编译成功${NC}"
echo ""

# 2. 检查示例可执行文件
echo "========================================"
echo "步骤 2: 检查示例可执行文件"
echo "========================================"

EXAMPLES_DIR="$BUILD_DIR/examples"
TOTAL=0
SUCCESS=0
FAILED=0

# 统一协议架构示例
echo ""
echo "【统一协议架构】"
for example in unified_simple_http unified_mixed_server unified_level2_demo unified_https_wss_server unified_async_demo unified_ws_client_test; do
    TOTAL=$((TOTAL + 1))
    if [ -f "$EXAMPLES_DIR/$example" ] && [ -x "$EXAMPLES_DIR/$example" ]; then
        echo -e "${GREEN}?${NC} $example"
        SUCCESS=$((SUCCESS + 1))
    else
        echo -e "${RED}?${NC} $example"
        FAILED=$((FAILED + 1))
    fi
done

# 异步响应示例
echo ""
echo "【异步响应】"
for example in simple_async_test async_http_demo; do
    TOTAL=$((TOTAL + 1))
    if [ -f "$EXAMPLES_DIR/$example" ] && [ -x "$EXAMPLES_DIR/$example" ]; then
        echo -e "${GREEN}?${NC} $example"
        SUCCESS=$((SUCCESS + 1))
    else
        echo -e "${RED}?${NC} $example"
        FAILED=$((FAILED + 1))
    fi
done

# 基础服务器示例
echo ""
echo "【基础服务器】"
for example in http_server https_server wss_server; do
    TOTAL=$((TOTAL + 1))
    if [ -f "$EXAMPLES_DIR/$example" ] && [ -x "$EXAMPLES_DIR/$example" ]; then
        echo -e "${GREEN}?${NC} $example"
        SUCCESS=$((SUCCESS + 1))
    else
        echo -e "${RED}?${NC} $example"
        FAILED=$((FAILED + 1))
    fi
done

# HTTP/2 示例
echo ""
echo "【HTTP/2】"
for example in h2_server h2_client http2_out_client; do
    TOTAL=$((TOTAL + 1))
    if [ -f "$EXAMPLES_DIR/$example" ] && [ -x "$EXAMPLES_DIR/$example" ]; then
        echo -e "${GREEN}?${NC} $example"
        SUCCESS=$((SUCCESS + 1))
    else
        echo -e "${RED}?${NC} $example"
        FAILED=$((FAILED + 1))
    fi
done

# WebSocket 示例
echo ""
echo "【WebSocket】"
for example in ws_broadcast_user ws_broadcast_periodic ws_bench_client ws_stickbridge_client; do
    TOTAL=$((TOTAL + 1))
    if [ -f "$EXAMPLES_DIR/$example" ] && [ -x "$EXAMPLES_DIR/$example" ]; then
        echo -e "${GREEN}?${NC} $example"
        SUCCESS=$((SUCCESS + 1))
    else
        echo -e "${RED}?${NC} $example"
        FAILED=$((FAILED + 1))
    fi
done

# 客户端示例
echo ""
echo "【客户端】"
for example in http_out_client biz_http_client router_client router_biz_client client_conn_factory_example; do
    TOTAL=$((TOTAL + 1))
    if [ -f "$EXAMPLES_DIR/$example" ] && [ -x "$EXAMPLES_DIR/$example" ]; then
        echo -e "${GREEN}?${NC} $example"
        SUCCESS=$((SUCCESS + 1))
    else
        echo -e "${RED}?${NC} $example"
        FAILED=$((FAILED + 1))
    fi
done

# 高级功能示例
echo ""
echo "【高级功能】"
for example in demo_multi_protocol_server demo_multi_thread_server http_server_close_demo http_close_demo; do
    TOTAL=$((TOTAL + 1))
    if [ -f "$EXAMPLES_DIR/$example" ] && [ -x "$EXAMPLES_DIR/$example" ]; then
        echo -e "${GREEN}?${NC} $example"
        SUCCESS=$((SUCCESS + 1))
    else
        echo -e "${RED}?${NC} $example"
        FAILED=$((FAILED + 1))
    fi
done

# 自定义协议示例
echo ""
echo "【自定义协议】"
for example in xproto_server xproto_client; do
    TOTAL=$((TOTAL + 1))
    if [ -f "$EXAMPLES_DIR/$example" ] && [ -x "$EXAMPLES_DIR/$example" ]; then
        echo -e "${GREEN}?${NC} $example"
        SUCCESS=$((SUCCESS + 1))
    else
        echo -e "${RED}?${NC} $example"
        FAILED=$((FAILED + 1))
    fi
done

# 3. 统计结果
echo ""
echo "========================================"
echo "测试结果统计"
echo "========================================"
echo "总计: $TOTAL 个示例"
echo -e "${GREEN}成功: $SUCCESS${NC}"
if [ $FAILED -gt 0 ]; then
    echo -e "${RED}失败: $FAILED${NC}"
else
    echo "失败: 0"
fi
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}? 所有示例都可用！${NC}"
    echo ""
    echo "========================================"
    echo "快速测试建议"
    echo "========================================"
    echo ""
    echo "1. 测试最简单的 HTTP 服务器："
    echo "   cd $EXAMPLES_DIR"
    echo "   ./unified_simple_http 8080"
    echo "   # 在另一个终端："
    echo "   curl http://127.0.0.1:8080/"
    echo ""
    echo "2. 测试异步响应和定时器："
    echo "   ./simple_async_test 8080"
    echo "   # 在另一个终端："
    echo "   curl http://127.0.0.1:8080/async"
    echo ""
    echo "3. 测试 Level 2 高级特性："
    echo "   ./unified_level2_demo 8080"
    echo "   curl http://127.0.0.1:8080/async"
    echo ""
    echo "更多信息请查看: examples/README_EXAMPLES_INDEX.md"
    echo ""
    exit 0
else
    echo -e "${RED}? 有 $FAILED 个示例不可用${NC}"
    echo "请检查编译错误信息"
    exit 1
fi

