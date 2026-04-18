// CacheManager.h
#pragma once
#include <string>
#include <mutex>
#include <unordered_map>
#include <memory>
#include "ShardedCache.h"
#include "RaftKVClient.h"

class HighAvailableCacheManager {
private:
    ShardedCache<std::string, std::string> cache_;
    
    // 【高并发优化】：使用分段锁 (Lock Striping) 替代无限增长的 key_locks_，防止内存泄漏并大幅降低锁冲突
    static const int STRIPE_COUNT = 256;
    std::mutex stripe_locks_[STRIPE_COUNT];
    
    RaftKVClient raft_client_;

    int get_random_jitter();
    size_t get_stripe_index(const std::string& key) {
        return std::hash<std::string>{}(key) % STRIPE_COUNT;
    }

public:
    HighAvailableCacheManager();
    std::string get_data(const std::string& key);
    bool write_data(const std::string& key, const std::string& value);
};
