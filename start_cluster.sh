#!/bin/bash

# 定义颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}=========================================${NC}"
echo -e "${YELLOW}🚀 正在启动高并发秒杀系统一键清理与启动脚本...${NC}"
echo -e "${YELLOW}=========================================${NC}"

# 1. 强力清理：杀掉所有可能残留的旧进程
echo -e "${YELLOW}🧹 正在清理残留进程与端口...${NC}"
# 杀掉名为 raft_node 和 gateway 的进程
killall -9 raft_node gateway 2>/dev/null
# 释放端口（防止 Nginx 或旧进程占用）
fuser -k 8080/tcp 8000/tcp 8001/tcp 8002/tcp 2>/dev/null

# 2. 数据重置：清理上次运行产生的脏数据（关键！）
echo -e "${YELLOW}💾 正在重置分布式账本 (.bin 文件)...${NC}"
rm -f build/*.bin
rm -f *.bin
# 清理旧日志
rm -rf logs/*
mkdir -p logs

# 3. 编译代码
echo -e "${YELLOW}🔨 正在并行编译代码...${NC}"
mkdir -p build
cd build
cmake .. && make -j$(nproc)

if [ $? -ne 0 ]; then
    echo -e "${RED}❌ 编译失败！请检查错误信息。${NC}"
    exit 1
fi
cd ..

# 定义关闭所有服务的函数（捕获 Ctrl+C）
function stop_all() {
    echo -e "\n${RED}🛑 正在关闭所有服务...${NC}"
    kill $GATEWAY_PID $RAFT_PID_0 $RAFT_PID_1 $RAFT_PID_2 2>/dev/null
    echo -e "${GREEN}👋 所有服务已安全退出。${NC}"
    exit
}

# 绑定 Ctrl+C 信号到 stop_all 函数
trap stop_all SIGINT

# 4. 启动 Raft 节点
echo -e "${GREEN}🌐 启动 Raft 节点集群 (0, 1, 2)...${NC}"
./build/raft_node 0 > logs/raft_0.log 2>&1 &
RAFT_PID_0=$!
./build/raft_node 1 > logs/raft_1.log 2>&1 &
RAFT_PID_1=$!
./build/raft_node 2 > logs/raft_2.log 2>&1 &
RAFT_PID_2=$!

# 等待 Raft 选主。在高负载下，建议至少等待 3-5 秒
echo -e "${YELLOW}⏳ 等待 Raft 集群选举 Leader (5秒)...${NC}"
sleep 5

# 5. 启动网关
echo -e "${GREEN}🚪 启动 API Gateway (8080)...${NC}"
./build/gateway > logs/gateway_console.log 2>&1 &
GATEWAY_PID=$!

echo -e "${YELLOW}=========================================${NC}"
echo -e "${GREEN}🎉 系统启动成功！${NC}"
echo -e "Raft PIDs: $RAFT_PID_0, $RAFT_PID_1, $RAFT_PID_2"
echo -e "Gateway PID: $GATEWAY_PID"
echo -e "${YELLOW}=========================================${NC}"
echo -e "📖 查看网关日志: ${CYAN}tail -f logs/gateway_console.log${NC}"
echo -e "🚀 运行压测工具: ${CYAN}./build/benchmark 100 1000${NC}"
echo -e "${YELLOW}=========================================${NC}"
echo -e "按 ${RED}[Ctrl+C]${NC} 或 ${RED}[Enter]${NC} 停止所有服务..."

read
stop_all