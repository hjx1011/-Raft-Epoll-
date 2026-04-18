#include <iostream>
#include <vector>
#include <thread>
#include <string>
#include "RaftNode.h"
#include "Persister.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/basic_file_sink.h"

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cout << "用法: ./raft_node <节点ID (0/1/2)>" << std::endl;
        return 1;
    }

    int myId = std::stoi(argv[1]);
    int nodeCount = 3;

    // 1. 配置 spdlog：每个节点生成独立的日志文件，并开启自动刷新
    std::string logName = "logs/raft_" + std::to_string(myId) + ".log";
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logName, true);
    
    auto logger = std::make_shared<spdlog::logger>("raft_node", spdlog::sinks_init_list{console_sink, file_sink});
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [node " + std::to_string(myId) + "][%^%l%$] %v");
    spdlog::flush_on(spdlog::level::info); // 【重要】确保每条 info 级别以上的日志都立刻写盘

    spdlog::info("===================================");
    spdlog::info("  启动 Raft 节点 {} ...", myId);
    spdlog::info("===================================");

    auto p = std::make_shared<Persister>(myId);
    RaftNode node(myId, nodeCount, p);

    std::vector<int> clusterPorts = {8000, 8001, 8002};
    node.setPeerPorts(clusterPorts);

    // 启动 RPC 服务器
    std::thread serverThread(&RaftNode::startRpcServer, &node);
    serverThread.detach();

    spdlog::info("--- Raft 控制台已就绪 ---");
    spdlog::info("请输入命令 (例如: SET name hjx)");

    std::string input;
    // 【重要修复】：确保这个循环能正常运行，不要在上面加死循环
    while (std::getline(std::cin, input)) {
        if (input.empty()) continue;

        // 【核心优化】：增加长度安全校验 (input.length() >= 4)，防止输入过短导致 substr 抛出越界异常崩溃
        if (input.length() >= 4 && input.substr(0, 4) == "snap") {
            node.snapshot(node.getLastLogIndex(), "KV_DB_SNAPSHOT_DATA");
        }
        // 统一支持大写或小写的 set
        else if (input.length() >= 3 && (input.substr(0, 3) == "set" || input.substr(0, 3) == "SET")) {
            
            // 【核心优化】：增加简单的格式校验，防止只输入 "set" 导致状态机解析出空 key 和 val
            if (input.length() > 4 && input[3] == ' ') {
                if (node.isLeader()) {
                    // 【核心修复】：只把 "set" 强转为大写的 "SET"，后面的 key 和 val 必须保持原样！
                    // 否则输入 "set myName hjx" 会被破坏成 "SET MYNAME HJX"
                    std::string cmd = "SET" + input.substr(3);
                    node.sendCommand(cmd);
                } else {
                    spdlog::warn("错误：我不是 Leader，请在 Leader 节点输入！");
                }
            } else {
                spdlog::warn("命令格式不完整，请使用: SET key value");
            }
            
        } else {
            spdlog::info("未知命令，请使用: SET key value");
        }
    }

    return 0;
}