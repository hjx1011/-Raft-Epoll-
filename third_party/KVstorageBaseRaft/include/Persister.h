#pragma once
#include <string>
#include <fstream>
#include <mutex>

class Persister {
private:
    std::mutex mtx;
    std::string raftState; // 序列化后的 Raft 核心状态 (term, votedFor, logs)
    std::string snapshot;  // 序列化后的数据库快照 (以后战役三再用)
    
    std::string stateFileName; // 存状态的文件名，比如 "raft_state_0.bin"

public:
    Persister(int nodeId) {
        stateFileName = "raft_state_" + std::to_string(nodeId) + ".bin";
    }

    // 将序列化后的数据写入硬盘
    void SaveRaftState(const std::string& state_data) {
        std::string data_to_write;
        {
            std::lock_guard<std::mutex> lock(mtx);
            raftState = state_data;
            data_to_write = state_data; // 拷贝一份出来用于写入
        } // 【核心性能优化】：提早释放锁！磁盘 I/O 极慢，如果在锁内写文件，会严重阻塞其他 gRPC 线程读内存！

        // 真正写入本地文件 (以二进制模式覆盖写入，移出锁外部)
        std::ofstream outFile(stateFileName, std::ios::binary | std::ios::trunc);
        if (outFile.is_open()) {
            outFile.write(data_to_write.c_str(), data_to_write.size());
            outFile.close();
        }
    }

    // 从硬盘读取数据
    std::string ReadRaftState() {
        std::lock_guard<std::mutex> lock(mtx);
        std::ifstream inFile(stateFileName, std::ios::binary);
        if (inFile.is_open()) {
            // 读取整个文件内容到 string 中
            raftState.assign((std::istreambuf_iterator<char>(inFile)),
                              std::istreambuf_iterator<char>());
            inFile.close();
        }
        return raftState;
    }
    
    // 获取当前状态的大小（以后用来判断是否需要做快照）
    long long RaftStateSize() {
        std::lock_guard<std::mutex> lock(mtx);
        return raftState.size();
    }
    
    // 同时保存 Raft 核心状态和快照数据
    void SaveStateAndSnapshot(const std::string& state_data, const std::string& snapshot_data) {
        std::string state_to_write, snap_to_write;
        {
            std::lock_guard<std::mutex> lock(mtx);
            raftState = state_data;
            snapshot = snapshot_data;
            state_to_write = state_data;
            snap_to_write = snapshot_data;
        } // 【核心性能优化】：同样提早释放锁

        // 存状态 (锁外执行 I/O)
        std::ofstream outFile(stateFileName, std::ios::binary | std::ios::trunc);
        if (outFile.is_open()) {
            outFile.write(state_to_write.c_str(), state_to_write.size());
            outFile.close();
        }
        
        // 存快照 (锁外执行 I/O)
        std::string snapFileName = "snapshot_" + stateFileName;
        std::ofstream snapFile(snapFileName, std::ios::binary | std::ios::trunc);
        if (snapFile.is_open()) {
            snapFile.write(snap_to_write.c_str(), snap_to_write.size());
            snapFile.close();
        }
    }

    // 读取快照数据
    std::string ReadSnapshot() {
        std::lock_guard<std::mutex> lock(mtx);
        std::string snapFileName = "snapshot_" + stateFileName;
        std::ifstream inFile(snapFileName, std::ios::binary);
        if (inFile.is_open()) {
            snapshot.assign((std::istreambuf_iterator<char>(inFile)),
                             std::istreambuf_iterator<char>());
            inFile.close();
        }
        return snapshot;
    }
};