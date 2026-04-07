#include <iostream>
#include <vector>
#include <thread>
#include <string>
#include "RaftNode.h"
#include "Persister.h"
#include "spdlog/spdlog.h"

int main(int argc, char* argv[]) {
    if (argc != 2) {
        spdlog::info("用法: ./my_server <节点ID (0/1/2)>\n");
        return 1;
    }

    int myId = std::stoi(argv[1]);
    int nodeCount = 3;

    if (myId < 0 || myId >= nodeCount) {
        spdlog::info("节点ID必须是 0, 1, 或 2\n");
        return 1;
    }

    spdlog::info("===================================\n");
    spdlog::info("  启动 Raft 节点 {} ...\n" ,myId);
    spdlog::info("===================================\n");

    auto p = std::make_shared<Persister>(myId);
    RaftNode node(myId, nodeCount, p);

    std::vector<int> clusterPorts = {8000, 8001, 8002};
    node.setPeerPorts(clusterPorts);

    // 【修复】：把 RPC 服务器放进后台线程运行，不阻塞主线程！
    std::thread serverThread(&RaftNode::startRpcServer, &node);
    serverThread.detach();

    // 【新增】：主线程变成交互式控制台
    spdlog::info("\n--- Raft 控制台已就绪 ---\n");
    spdlog::info("输入 'set key value' 发送命令\n");
    spdlog::info("输入 'snap' 强制生成快照\n");

    std::string input;
    while (std::getline(std::cin, input)) {
        if (input.substr(0, 4) == "snap") {
            // 模拟触发快照：强制把当前已提交的日志截断
            std::string fakeSnapshotData = "KV_DB_DATA: SNAPSHOT_TEST"; 
            // 注意：这里为了测试，我们直接截断到当前节点拥有的最新日志
            node.snapshot(node.getLastLogIndex(), fakeSnapshotData);
            continue;
        }
        
        if (input.substr(0, 3) == "set") {
            if (node.isLeader()) {
                node.sendCommand(input);
            } else {
                spdlog::info("错误：我不是 Leader，请找 Leader 节点！\n");
            }
        }
    }

    return 0;
}