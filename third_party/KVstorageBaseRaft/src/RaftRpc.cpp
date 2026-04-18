#include "RaftRpc.h"
#include "RaftNode.h"
#include "raft.grpc.pb.h" // gRPC 自动生成的头文件
#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <mutex>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::ClientContext;

// ================= 1. gRPC 服务端实现 =================
class RaftServiceImpl final : public raft::RaftService::Service {
    RaftNode* node_;
public:
    RaftServiceImpl(RaftNode* node) : node_(node) {}

    Status RequestVote(ServerContext* context, const raft::RequestVoteArgs* request, raft::RequestVoteReply* reply) override {
        // 将 gRPC 请求中的 4 个字段全部提取出来，填入你的 C++ 结构体
        RequestVoteArgs args;
        args.term = request->term();
        args.candidateId = request->candidateid();
        args.lastLogIndex = request->lastlogindex(); // 【新增】
        args.lastLogTerm = request->lastlogterm();   // 【新增】

        // 调用 RaftNode 的逻辑处理
        RequestVoteReply res = node_->handleRequestVote(args);

        // 返回结果
        reply->set_term(res.term);
        reply->set_votegranted(res.voteGranted);
        return Status::OK;
    }

    Status AppendEntries(ServerContext* context, const raft::AppendEntriesArgs* request, raft::AppendEntriesReply* reply) override {
        AppendEntriesArgs args;
        args.term = request->term();
        args.leaderId = request->leaderid();
        args.prevLogIndex = request->prevlogindex();
        args.prevLogTerm = request->prevlogterm();
        args.leaderCommit = request->leadercommit();
        
        // 【核心优化】：预分配内存，避免 vector 扩容时的多次拷贝开销
        args.entries.reserve(request->entries_size());
        for (int i = 0; i < request->entries_size(); i++) {
            // 【核心优化】：使用 emplace_back 原地构造，避免产生临时对象
            args.entries.emplace_back(request->entries(i).term(), request->entries(i).command());
        }
        
        AppendEntriesReply res = node_->handleAppendEntries(args);
        reply->set_term(res.term);
        reply->set_success(res.success);
        return Status::OK;
    }

    Status InstallSnapshot(ServerContext* context, const raft::InstallSnapshotArgs* request, raft::InstallSnapshotReply* reply) override {
        InstallSnapshotArgs args{request->term(), request->leaderid(), request->lastincludedindex(), request->lastincludedterm(), request->data()};
        InstallSnapshotReply res = node_->handleInstallSnapshot(args);
        reply->set_term(res.term);
        return Status::OK;
    }

    Status AttemptLock(ServerContext* context, const raft::LockArgs* request, raft::LockReply* reply) override {
        reply->set_code(node_->attemptLock(request->resource(), request->clientid()));
        return Status::OK;
    }

    Status ReleaseLock(ServerContext* context, const raft::UnlockArgs* request, raft::UnlockReply* reply) override {
        node_->releaseLock(request->resource());
        reply->set_success(true);
        return Status::OK;
    }
};

void RaftRpc::startRpcServer(int myPort, RaftNode* node, bool& running) {
    std::string server_address("0.0.0.0:" + std::to_string(myPort));
    RaftServiceImpl service(node);
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    spdlog::info("🌐 gRPC 服务器已启动，监听端口 {}", myPort);
    server->Wait(); // 阻塞运行
}

// ================= 2. gRPC 客户端实现 (自带多路复用连接池) =================
static std::unordered_map<int, std::shared_ptr<raft::RaftService::Stub>> g_stubs;
static std::mutex g_stub_mtx;

// 获取 gRPC Stub (底层自动维护 HTTP/2 长连接)
std::shared_ptr<raft::RaftService::Stub> getStub(int port) {
    std::lock_guard<std::mutex> lock(g_stub_mtx);
    if (g_stubs.find(port) == g_stubs.end()) {
        auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        g_stubs[port] = raft::RaftService::NewStub(channel);
    }
    return g_stubs[port];
}

// 【核心修复】：入参改为 const RequestVoteArgs&，与头文件保持一致，消除编译报错并减少拷贝
RequestVoteReply RaftRpc::callRequestVote(int targetPort, const RequestVoteArgs& args, bool& success) {
    raft::RequestVoteArgs req;
    // 将 C++ 结构体中的 4 个字段填入 gRPC 的请求包中
    req.set_term(args.term);
    req.set_candidateid(args.candidateId);
    req.set_lastlogindex(args.lastLogIndex); // 【新增】
    req.set_lastlogterm(args.lastLogTerm);   // 【新增】

    raft::RequestVoteReply rep;
    ClientContext context;
    // 设置 100ms 超时，防止网络卡死
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(100));

    Status status = getStub(targetPort)->RequestVote(&context, req, &rep);
    if (status.ok()) {
        success = true;
        return RequestVoteReply{rep.term(), rep.votegranted()};
    }
    success = false;
    return RequestVoteReply{};
}

// 【核心修复】：入参改为 const AppendEntriesArgs&
AppendEntriesReply RaftRpc::callAppendEntries(int targetPort, const AppendEntriesArgs& args, bool& success) {
    raft::AppendEntriesArgs req;
    req.set_term(args.term);
    req.set_leaderid(args.leaderId);
    req.set_prevlogindex(args.prevLogIndex);
    req.set_prevlogterm(args.prevLogTerm);
    req.set_leadercommit(args.leaderCommit);
    
    // 【核心优化】：让 Protobuf 底层预分配内存，防止大批量日志同步时频繁申请内存
    req.mutable_entries()->Reserve(args.entries.size());
    for (const auto& e : args.entries) {
        auto* entry = req.add_entries();
        entry->set_term(e.term);
        entry->set_command(e.command);
    }
    
    raft::AppendEntriesReply rep;
    ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(150)); // 150ms 超时

    Status status = getStub(targetPort)->AppendEntries(&context, req, &rep);
    if (status.ok()) {
        success = true;
        return AppendEntriesReply{rep.term(), rep.success()};
    }
    success = false;
    return AppendEntriesReply{};
}

// 【核心修复】：入参改为 const InstallSnapshotArgs&
InstallSnapshotReply RaftRpc::callInstallSnapshot(int targetPort, const InstallSnapshotArgs& args, bool& success) {
    raft::InstallSnapshotArgs req;
    req.set_term(args.term);
    req.set_leaderid(args.leaderId);
    req.set_lastincludedindex(args.lastIncludedIndex);
    req.set_lastincludedterm(args.lastIncludedTerm);
    req.set_data(args.data);
    raft::InstallSnapshotReply rep;
    ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500)); 

    Status status = getStub(targetPort)->InstallSnapshot(&context, req, &rep);
    if (status.ok()) {
        success = true;
        return InstallSnapshotReply{rep.term()};
    }
    success = false;
    return InstallSnapshotReply{};
}