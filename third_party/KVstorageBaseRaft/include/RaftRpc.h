#pragma once
#include <string>
#include <vector>
#include "RaftTypes.h"

class RaftNode;

class RaftRpc {
public:
    // 客户端：发送 RPC 请求 (自带超时控制)
    static RequestVoteReply callRequestVote(int targetPort, const RequestVoteArgs& args, bool& success);
    static AppendEntriesReply callAppendEntries(int targetPort, const AppendEntriesArgs& args, bool& success);
    static InstallSnapshotReply callInstallSnapshot(int targetPort, const InstallSnapshotArgs& args, bool& success);

    // 服务端：启动 gRPC 监听
    static void startRpcServer(int myPort, RaftNode* node, bool& running);
};