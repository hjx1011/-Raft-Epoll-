#pragma once
#include "raft.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <string>
#include <vector>
#include <memory>
#include <unistd.h>
#include <unordered_map>
#include <mutex>

class RaftKVClient {
private:
    std::vector<int> ports = {8000, 8001, 8002};
    
    // 【修复】增加 Stub 缓存池，防止高并发下疯狂创建 Channel 导致 FD 耗尽
    std::unordered_map<int, std::shared_ptr<raft::RaftService::Stub>> stubs_;
    std::mutex mtx_;

    std::shared_ptr<raft::RaftService::Stub> getStub(int port) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (stubs_.find(port) == stubs_.end()) {
            auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
            stubs_[port] = raft::RaftService::NewStub(channel);
        }
        return stubs_[port];
    }

public:
    bool tryLock(const std::string& resource, const std::string& clientId) {
        for (int i = 0; i < 3; i++) {
            for (int port : ports) {
                raft::LockArgs req;
                req.set_resource(resource);
                req.set_clientid(clientId);
                raft::LockReply rep;
                grpc::ClientContext context;
                context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(100));
                
                grpc::Status status = getStub(port)->AttemptLock(&context, req, &rep);
                if (status.ok()) {
                    if (rep.code() == 0) return true;  
                    if (rep.code() == 1) return false; 
                }
            }
            usleep(100000); 
        }
        return false;
    }

    void unlock(const std::string& resource) {
        for (int port : ports) {
            raft::UnlockArgs req;
            req.set_resource(resource);
            raft::UnlockReply rep;
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(50));
            getStub(port)->ReleaseLock(&context, req, &rep);
        }
    }
};