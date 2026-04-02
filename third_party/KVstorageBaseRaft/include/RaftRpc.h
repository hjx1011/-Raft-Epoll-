#pragma once
#include <string>
#include <vector>
#include "RaftTypes.h"

class RaftNode; // 前向声明

class RaftRpc {
public:
    // --- 序列化/反序列化工具 ---
    static std::string serializeRequestVoteArgs(RequestVoteArgs args);
    static RequestVoteReply deserializeRequestVoteReply(std::string data);
    
    static std::string serializeAppendEntriesArgs(AppendEntriesArgs args);
    static AppendEntriesReply deserializeAppendEntriesReply(std::string data);

    // --- 客户端：发送 RPC 请求 ---
    static std::string sendTcpRequest(int targetPort, std::string requestData);

    // 封装好的高级接口
    static RequestVoteReply callRequestVote(int targetPort, RequestVoteArgs args, bool& success);
    static AppendEntriesReply callAppendEntries(int targetPort, AppendEntriesArgs args, bool& success);

    // --- 服务端：启动 RPC 监听 ---
    static void startRpcServer(int myPort, RaftNode* node, bool& running);

    // 在 public 区域加入：
    static std::string serializeInstallSnapshotArgs(InstallSnapshotArgs args);
    static InstallSnapshotReply deserializeInstallSnapshotReply(std::string data);
    static InstallSnapshotReply callInstallSnapshot(int targetPort, InstallSnapshotArgs args, bool& success);
};