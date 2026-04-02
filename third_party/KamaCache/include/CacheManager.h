// CacheManager.h
#pragma once
#include <string>
#include <mutex>
#include <unordered_map>
#include <memory>
#include "ShardedCache.h"
#include "RaftKVClient.h" // 【新增】

class HighAvailableCacheManager {
private:
    ShardedCache<std::string, std::string> cache_;
    std::mutex loader_mtx_;
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> key_locks_;
    
    RaftKVClient raft_client_; // 【新增】：Raft 客户端

    int get_random_jitter();

public:
    HighAvailableCacheManager();
    std::string get_data(const std::string& key);
    
    // 【新增】：处理写请求 (Cache Aside 模式)
    bool write_data(const std::string& key, const std::string& value);
};