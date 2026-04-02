#pragma once
#include "RaftRpc.h"
#include <string>
#include <vector>
#include <unistd.h>

class RaftKVClient {
private:
    std::vector<int> ports = {8000, 8001, 8002};

public:
    // 抢锁：内部会自动轮询寻找 Leader
    bool tryLock(const std::string& resource, const std::string& clientId) {
        for (int i = 0; i < 3; i++) { // 尝试三次抢锁
            for (int port : ports) {
                std::string req = "CLI_LOCK " + resource + " " + clientId;
                std::string rep = RaftRpc::sendTcpRequest(port, req);
                if (rep == "LOCK_REP\n0") return true;   // 抢锁成功
                if (rep == "LOCK_REP\n1") return false;  // 锁被占用
                // 如果返回 2，说明不是 Leader，继续找下一个端口
            }
            usleep(100000); // 100ms 后重试
        }
        return false;
    }

    void unlock(const std::string& resource) {
        for (int port : ports) {
            std::string req = "CLI_UNLOCK " + resource;
            RaftRpc::sendTcpRequest(port, req);
            // 简单起见，广播解锁指令，只有 Leader 会处理
        }
    }
};