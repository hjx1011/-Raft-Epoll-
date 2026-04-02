#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <chrono>
#include <random>
#include "RaftTypes.h" 
#include "Persister.h"
#include <memory>
#include <unordered_map>

// 注意：这里不需要引入 socket 相关的头文件了，因为网络层已经被剥离到了 RaftRpc 中！
// 也不需要 #include "RaftRpc.h"，在 RaftNode.cpp 里引入即可，保持头文件清爽。

class RaftNode {
private:
    // --- 互斥锁 ---
    std::mutex mtx; // Raft 是多线程并发模型，任何对节点状态（如 term, state, logs）的访问都必须加锁

    // --- 持久化状态 (Persistent State) ---
    int currentTerm = 0;              // 服务器见过的最新任期号
    int votedFor = -1;                // 在当前任期内，本节点投票给了谁（ID），-1 表示还没投
    std::vector<LogEntry> logs;       // 日志条目数组，保存所有操作指令

    // --- 所有节点上的易失性状态 (Volatile State) ---
    int commitIndex = 0;              // 已知已提交（Over Majority）的最高日志索引
    int lastApplied = 0;              // 已经被应用到状态机（如 KV 数据库）的最高日志索引

    // --- Leader 专属的易失性状态 (每次选举后重新初始化) ---
    std::vector<int> nextIndex;       // 大哥视角：下一次发给各个小弟的日志索引
    std::vector<int> matchIndex;      // 大哥视角：各个小弟已经同步好的最高日志索引

    // --- 节点自身基本信息 ---
    int me;                           // 本节点在集群里的唯一 ID
    int peerCount;                    // 集群总节点数
    NodeState state = NodeState::FOLLOWER; // 节点当前身份：跟随者、候选人、或领导者

    // --- 定时炸弹（选举超时）相关 ---
    std::chrono::time_point<std::chrono::system_clock> lastHeartBeat; // 上一次收到大哥心跳的时间
    std::thread tickerThread;         // 后台倒计时线程，负责触发选举
    std::mt19937 randomGenerator;     // 随机数生成器，用于生成 150ms~300ms 的随机超时时间

    bool running = true;              // 控制所有后台线程运行的标志位
    int currentVotes = 0;             // 竞选期间获得的选票计数器

    // --- 网络端口信息 ---
    std::vector<int> peerPorts;       // 集群中所有节点的端口号数组
    int myPort;                       // 本节点的端口号

    std::shared_ptr<Persister> persister; // 【新增】持久化引擎的指针

    std::unordered_map<std::string, std::string> kv_db;

    int lastIncludedIndex = 0; // 快照替代的最高日志条目的索引
    int lastIncludedTerm = 0;  // 快照替代的最高日志条目的任期

    void ticker();

    void startElection();

    bool sendRequestVote(int targetId, RequestVoteArgs args);

    void becomeLeader();

    void startHeartbeatTimer();

    void sendHeartbeats();

    void updateCommitIndex(); 

    // 【新增】两个私有方法，负责把状态打包和解包
    void persist(); 
    void readPersist(std::string data);
    void applyLogs();

        // 获取某条日志的真实索引
    int getAbsoluteIndex(int vectorIndex) {
        return vectorIndex + lastIncludedIndex;
    }
    
    // 根据真实索引，获取它在 logs 数组里的下标
    int getVectorIndex(int absoluteIndex) {
        return absoluteIndex - lastIncludedIndex;
    }

    
public: 

    RaftNode(int myId, int peers, std::shared_ptr<Persister> p);

    ~RaftNode();

    AppendEntriesReply handleAppendEntries(AppendEntriesArgs args);

    RequestVoteReply handleRequestVote(RequestVoteArgs args);

    void setPeerPorts(std::vector<int>& ports);

    void startRpcServer();

    bool isLeader();

    void sendCommand(std::string cmd);

    //[RPC 处理] 处理大哥发来的快照安装请求
    InstallSnapshotReply handleInstallSnapshot(InstallSnapshotArgs args);

    // [外部接口] 状态机主动调用：告诉 Raft 节点“我已经把状态存下来了，你可以把 index 之前的日志删了”
    void snapshot(int index, std::string snapshotData);

    // 获取当前节点拥有的最后一条日志的真实索引
    int getLastLogIndex() {
        return lastIncludedIndex + logs.size() - 1;
    }

    // 获取当前节点拥有的最后一条日志的任期
    int getLastLogTerm() {
        if (logs.size() == 1) { // 如果数组里只剩一个占位符，说明全被快照了
            return lastIncludedTerm;
        } else {
            return logs.back().term;
        }
    }

    // --- 新增：供外部网关调用的接口 ---
    std::string getKV(const std::string& key) {
        std::lock_guard<std::mutex> lock(mtx);
        if (kv_db.count(key)) return kv_db[key];
        return "";
    }

    bool setKV(const std::string& key, const std::string& val) {
        std::lock_guard<std::mutex> lock(mtx);
        if (state != NodeState::LEADER) return false; // 只有大哥能处理写请求
        
        // 包装成命令写入日志，等待心跳同步并 Commit
        std::string cmd = "SET " + key + " " + val;
        logs.push_back(LogEntry(currentTerm, cmd));
        persist();
        return true;
    }

    // 尝试原子加锁
    // 返回 0: 成功, 1: 锁已被占用, 2: 不是 Leader
    int attemptLock(const std::string& resource, const std::string& clientId) {
        std::lock_guard<std::mutex> lock(mtx);
        if (state != NodeState::LEADER) return 2;

        // 检查锁是否已被占用 (假设锁在 KV 中以 "lock:" 为前缀)
        std::string lockKey = "LOCK_" + resource;
        if (kv_db.count(lockKey)) return 1; 

        // 锁没被占用，发起一条 Raft 日志进行同步
        std::string cmd = "SET " + lockKey + " " + clientId;
        logs.push_back(LogEntry(currentTerm, cmd));
        persist();
        return 0;
    }

    // 释放锁
    void releaseLock(const std::string& resource) {
        std::lock_guard<std::mutex> lock(mtx);
        if (state != NodeState::LEADER) return;
        
        std::string lockKey = "LOCK_" + resource;
        std::string cmd = "DEL " + lockKey; // 我们假设状态机能处理 DEL 命令
        logs.push_back(LogEntry(currentTerm, cmd));
        persist();
    }
};
//Append附加，多余
//Entries 条目
//Reply 回复
//Request 请求
//Args 参数