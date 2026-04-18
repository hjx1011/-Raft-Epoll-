#pragma once // 防止头文件被重复包含
#include <string>
#include <vector>

// 定义日志条目
struct LogEntry {
    // 【核心优化】：默认初始化 = 0，防止未初始化导致内存脏数据问题
    int term = 0;       // 这条日志是在哪个任期被 Leader 接收的
    std::string command; // 具体的命令内容（暂时用 string 代替，后期我们会换成具体的 KV 操作指令）
    
    // 构造函数
    // 【核心优化】：引入 std::move 避免字符串传参时的深拷贝，提升序列化性能
    LogEntry(int t, std::string cmd) : term(t), command(std::move(cmd)) {}
};

// 定义节点的三个身份状态
enum class NodeState {
    FOLLOWER,   // 跟随者（小弟）
    CANDIDATE,  // 候选人（想当大哥）
    LEADER      // 领导者（大哥）
};

// 大哥发的包：AppendEntries (现在不仅是心跳，还要带上日志)
struct AppendEntriesArgs {
    int term = 0;         // 大哥当前的任期
    int leaderId = 0;     // 大哥是谁
    
    // --- 新增的日志复制字段 ---
    int prevLogIndex = 0;     // 紧挨着新日志的前一条日志的索引
    int prevLogTerm = 0;      // 紧挨着新日志的前一条日志的任期  
    std::vector<LogEntry> entries; // 准备复制过去的日志（如果是纯心跳，这个数组就是空的）
    int leaderCommit = 0;     // 大哥已经提交的日志索引
};

// 小弟回的包
struct AppendEntriesReply {
    int term = 0;         // 小弟现在的任期（万一小弟任期更高，大哥就得下台）
    bool success = false; // 小弟服不服（是否接收了这条日志）
};

// 候选人发起的拉票请求包
struct RequestVoteArgs {
    int term = 0;         // 候选人的任期
    int candidateId = 0;  // 候选人的 ID（我是谁，请投给我）
    int lastLogIndex = 0; 
    int lastLogTerm = 0; 
    // 真实的 Raft 还有 lastLogIndex 和 lastLogTerm，为了循序渐进，我们先省略
};

// 目标节点回复的投票结果包
struct RequestVoteReply {
    int term = 0;         // 目标节点的当前任期（如果比候选人高，候选人就得乖乖退选）
    bool voteGranted = false; // 是否同意投票给该候选人
};

// 大哥发给落后小弟的“快照包”
struct InstallSnapshotArgs {
    int term = 0;              // 大哥的任期
    int leaderId = 0;          // 大哥的 ID
    int lastIncludedIndex = 0; // 快照包含的最后一条日志的真实索引
    int lastIncludedTerm = 0;  // 快照包含的最后一条日志的任期
    std::string data;      // 真正的快照数据（比如序列化后的 KV 数据库）
};

// 小弟的回复包
struct InstallSnapshotReply {
    int term = 0;              // 小弟的任期（用于让过期大哥退位）
};