#!/bin/bash

# MyFrame 股票系统演示脚本
# 展示三个服务器之间的通信

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build/examples"

echo "=== MyFrame 股票系统演示 ==="
echo "此演示启动三个服务器："
echo "  - stock_index_server (端口7801)  - 股票索引服务"
echo "  - stock_bridge_server (端口7802) - 股票桥接服务"  
echo "  - stock_driver (端口7800)        - 驱动程序（连接前两个服务）"
echo ""

# 检查可执行文件
for prog in stock_index_server stock_bridge_server stock_driver; do
    if [[ ! -x "${BUILD_DIR}/${prog}" ]]; then
        echo "错误: ${prog} 不存在或不可执行"
        echo "请先运行: ./scripts/build_cmake.sh all"
        exit 1
    fi
done

# 清理函数
cleanup() {
    echo ""
    echo "正在停止所有服务..."
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    wait 2>/dev/null || true
    echo "演示结束"
}

# 设置信号处理
PIDS=()
trap cleanup EXIT INT TERM

echo "启动股票索引服务器 (端口7801)..."
cd "${BUILD_DIR}"
./stock_index_server 7801 &
PIDS+=($!)
sleep 1

echo "启动股票桥接服务器 (端口7802)..."
./stock_bridge_server 7802 &
PIDS+=($!)
sleep 1

echo "启动股票驱动程序 (端口7800)..."
./stock_driver 7800 &
PIDS+=($!)
sleep 2

echo ""
echo "? 所有服务已启动!"
echo ""
echo "? 测试命令:"
echo "  # 查看股票索引服务状态"
echo "  curl http://127.0.0.1:7801/api/status"
echo ""
echo "  # 获取股票符号列表"
echo "  curl http://127.0.0.1:7801/symbols"
echo ""
echo "  # 查看股票桥接服务状态"
echo "  curl http://127.0.0.1:7802/api/status"
echo ""
echo "  # 查看驱动程序状态"
echo "  curl http://127.0.0.1:7800/api/status"
echo ""
echo "  # WebSocket 测试 (需要 websocat)"
echo "  websocat ws://127.0.0.1:7801/websocket"
echo "  websocat ws://127.0.0.1:7802/websocket"
echo ""

# 自动测试
echo "? 自动测试中..."
sleep 2

echo ""
echo "测试股票索引服务:"
curl -s http://127.0.0.1:7801/api/status | python3 -m json.tool 2>/dev/null || curl -s http://127.0.0.1:7801/api/status

echo ""
echo "测试股票符号列表:"
curl -s http://127.0.0.1:7801/symbols | python3 -m json.tool 2>/dev/null || curl -s http://127.0.0.1:7801/symbols

echo ""
echo "测试股票桥接服务:"
curl -s http://127.0.0.1:7802/api/status | python3 -m json.tool 2>/dev/null || curl -s http://127.0.0.1:7802/api/status

echo ""
echo "测试驱动程序状态:"
curl -s http://127.0.0.1:7800/api/status | python3 -m json.tool 2>/dev/null || curl -s http://127.0.0.1:7800/api/status

echo ""
echo "? 基本功能测试完成!"
echo ""
echo "按 Ctrl+C 停止所有服务..."

# 保持运行直到用户中断
while true; do
    sleep 1
    # 检查进程是否还在运行
    for i in "${!PIDS[@]}"; do
        if ! kill -0 "${PIDS[$i]}" 2>/dev/null; then
            echo "警告: 进程 ${PIDS[$i]} 已退出"
            unset "PIDS[$i]"
        fi
    done
    
    # 如果所有进程都退出了，也退出脚本
    if [[ ${#PIDS[@]} -eq 0 ]]; then
        echo "所有服务都已退出"
        break
    fi
done
