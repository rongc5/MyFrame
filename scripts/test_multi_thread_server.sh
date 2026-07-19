#!/bin/bash

# MyFrame 多线程服务器自动化测试脚本
# 用法: ./test_multi_thread_server.sh [端口] [线程数]

set -euo pipefail

PORT="${1:-9998}"
THREADS="${2:-4}"
SERVER_URL="http://127.0.0.1:${PORT}"

echo "=== MyFrame 多线程服务器测试脚本 ==="
echo "测试端口: $PORT"
echo "线程数: $THREADS"
echo "服务器URL: $SERVER_URL"
echo

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 检查服务器是否运行
check_server() {
    echo -n "检查服务器是否运行... "
    if curl -s --connect-timeout 3 "$SERVER_URL/api/thread-info" > /dev/null; then
        echo -e "${GREEN}✓ 服务器运行中${NC}"
        return 0
    else
        echo -e "${RED}✗ 服务器未响应${NC}"
        echo "请先启动服务器:"
        echo "cd /home/rong/myframe/build/examples"
        echo "./demo_multi_thread_server $PORT $THREADS &"
        exit 1
    fi
}

# 测试基本接口
test_basic_apis() {
    echo
    echo "=== 1. 基本API测试 ==="
    
    echo -n "测试线程信息接口... "
    THREAD_INFO=$(curl -s "$SERVER_URL/api/thread-info")
    if echo "$THREAD_INFO" | jq -e '.current_thread' > /dev/null 2>&1; then
        echo -e "${GREEN}✓ 通过${NC}"
        THREAD_COUNT=$(echo "$THREAD_INFO" | jq -r '.thread_count')
        echo "  CPU核心数: $THREAD_COUNT"
    else
        echo -e "${RED}✗ 失败${NC}"
        exit 1
    fi
    
    echo -n "测试统计信息接口... "
    STATS=$(curl -s "$SERVER_URL/api/thread-stats")
    if echo "$STATS" | jq -e '.total_requests' > /dev/null 2>&1; then
        echo -e "${GREEN}✓ 通过${NC}"
        TOTAL_REQ=$(echo "$STATS" | jq -r '.total_requests')
        WORKER_COUNT=$(echo "$STATS" | jq -r '.threads | length')
        echo "  总请求数: $TOTAL_REQ, 工作线程数: $WORKER_COUNT"
    else
        echo -e "${RED}✗ 失败${NC}"
        exit 1
    fi
}

# 重置统计
reset_stats() {
    echo
    echo "=== 2. 重置统计信息 ==="
    echo -n "重置统计... "
    RESET_RESULT=$(curl -s -X POST "$SERVER_URL/api/reset-stats")
    if echo "$RESET_RESULT" | jq -e '.status' | grep -q "reset"; then
        echo -e "${GREEN}✓ 重置成功${NC}"
    else
        echo -e "${YELLOW}⚠ 重置可能失败，继续测试${NC}"
    fi
}

# 并发测试
test_concurrency() {
    echo
    echo "=== 3. 并发负载均衡测试 ==="
    
    local concurrent_requests=20
    echo "发送 $concurrent_requests 个并发请求..."
    
    # 并发发送请求
    for i in $(seq 1 $concurrent_requests); do
        curl -s "$SERVER_URL/api/load-test" > /dev/null &
    done
    wait
    
    # 等待处理完成
    sleep 1
    
    # 检查结果
    echo -n "分析负载分布... "
    STATS=$(curl -s "$SERVER_URL/api/thread-stats")
    TOTAL_REQ=$(echo "$STATS" | jq -r '.total_requests')
    
    if [ "$TOTAL_REQ" -ge "$concurrent_requests" ]; then
        echo -e "${GREEN}✓ 请求处理完成${NC}"
        echo "总请求数: $TOTAL_REQ"
        
        echo "各线程请求分布:"
        echo "$STATS" | jq -r '.threads[]' | while read -r thread_stat; do
            echo "  $thread_stat"
        done
        
        # 检查负载均衡
        MIN_REQUESTS=$(echo "$STATS" | jq -r '.threads[]' | grep -o 'Req=[0-9]*' | sed 's/Req=//' | sort -n | head -1)
        MAX_REQUESTS=$(echo "$STATS" | jq -r '.threads[]' | grep -o 'Req=[0-9]*' | sed 's/Req=//' | sort -n | tail -1)
        
        if [ -n "$MIN_REQUESTS" ] && [ -n "$MAX_REQUESTS" ]; then
            DIFF=$((MAX_REQUESTS - MIN_REQUESTS))
            if [ "$DIFF" -le $((concurrent_requests / 2)) ]; then
                echo -e "${GREEN}✓ 负载均衡良好 (最大差异: $DIFF)${NC}"
            else
                echo -e "${YELLOW}⚠ 负载分布不够均匀 (最大差异: $DIFF)${NC}"
            fi
        fi
        
    else
        echo -e "${RED}✗ 请求处理不完整${NC}"
        exit 1
    fi
}

# 压力测试
test_stress() {
    echo
    echo "=== 4. 压力测试 ==="
    
    if command -v ab > /dev/null; then
        echo "使用 Apache Bench 进行压力测试..."
        echo "ab -n 100 -c 5 -q $SERVER_URL/api/load-test"
        ab -n 100 -c 5 -q "$SERVER_URL/api/load-test"
        
        # 检查结果
        sleep 1
        STATS=$(curl -s "$SERVER_URL/api/thread-stats")
        TOTAL_REQ=$(echo "$STATS" | jq -r '.total_requests')
        echo -e "${GREEN}✓ 压力测试完成，总请求数: $TOTAL_REQ${NC}"
    else
        echo -e "${YELLOW}⚠ 未安装 Apache Bench，跳过压力测试${NC}"
        echo "安装方法: sudo apt-get install apache2-utils"
    fi
}

# 响应时间测试
test_response_time() {
    echo
    echo "=== 5. 响应时间测试 ==="
    
    echo "测试响应时间..."
    TIMES=()
    for i in $(seq 1 5); do
        TIME=$(curl -w "%{time_total}" -o /dev/null -s "$SERVER_URL/api/load-test")
        TIMES+=("$TIME")
        echo "  第${i}次: ${TIME}s"
    done
    
    # 计算平均值
    TOTAL=0
    for time in "${TIMES[@]}"; do
        TOTAL=$(echo "$TOTAL + $time" | bc -l)
    done
    AVG=$(echo "scale=4; $TOTAL / ${#TIMES[@]}" | bc -l)
    echo -e "${GREEN}✓ 平均响应时间: ${AVG}s${NC}"
}

# 最终报告
final_report() {
    echo
    echo "=== 最终测试报告 ==="
    
    STATS=$(curl -s "$SERVER_URL/api/thread-stats")
    TOTAL_REQ=$(echo "$STATS" | jq -r '.total_requests')
    WORKER_COUNT=$(echo "$STATS" | jq -r '.threads | length')
    
    echo "服务器信息:"
    echo "  端口: $PORT"
    echo "  配置线程数: $THREADS"  
    echo "  实际工作线程数: $WORKER_COUNT"
    echo "  总处理请求数: $TOTAL_REQ"
    echo
    echo "线程负载分布:"
    echo "$STATS" | jq -r '.threads[]' | while read -r thread_stat; do
        echo "  $thread_stat"
    done
    
    echo
    echo -e "${GREEN}🎉 所有测试完成！${NC}"
    echo
    echo "实时监控命令:"
    echo "  watch -n 1 'curl -s $SERVER_URL/api/thread-stats | jq .'"
    echo
    echo "停止服务器:"
    echo "  pkill -f demo_multi_thread_server"
}

# 主测试流程
main() {
    check_server
    test_basic_apis
    reset_stats
    test_concurrency
    test_stress
    test_response_time
    final_report
}

# 错误处理
trap 'echo -e "\n${RED}测试被中断${NC}"; exit 1' INT TERM

# 执行测试
main "$@"