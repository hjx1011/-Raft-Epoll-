#include "CacheManager.h"
#include "MySQLClient.h"
#include <iostream>
#include <random>
#include <thread>

// 全局 MySQL 客户端
MySQLClient g_mysql;

auto global_eviction_callback = [](const std::string& k, const std::string& v) {
    std::cout << "  🗑️ [Cache 淘汰] Key: " << k << ", Val: " << v << std::endl;
};

HighAvailableCacheManager::HighAvailableCacheManager() 
    : cache_(16, 10, global_eviction_callback) {}

// 【防雪崩】：获取 0-60 秒的随机抖动，防止缓存同时大面积失效
int HighAvailableCacheManager::get_random_jitter() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 60);
    return dis(gen);
}

// 【分布式一致性写】：Raft 锁保护下的 MySQL 写入
bool HighAvailableCacheManager::write_data(const std::string& key, const std::string& value) {
    std::string clientId = "Gateway_Node_1";
    
    // 1. 抢分布式锁：保证全球只有一个网关在改这个 key
    if (raft_client_.tryLock(key, clientId)) {
        
        // 2. 写入 MySQL (Source of Truth)
        if (g_mysql.upsert(key, value)) {
            
            // 3. 淘汰缓存 (Cache Aside 模式通常选择删除或覆盖，这里选择覆盖并带上随机 TTL)
            cache_.put(key, value, 3600 + get_random_jitter());
            
            // 4. 释放 Raft 锁
            raft_client_.unlock(key);
            return true;
        }
        raft_client_.unlock(key);
    }
    return false;
}

// 【读取路径】：完整保留防击穿、防穿透、防雪崩逻辑
std::string HighAvailableCacheManager::get_data(const std::string& key) {
    std::string val;
    
    // --- 步骤 1：直接查缓存 ---
    if (cache_.get(key, val)) {
        // 【防穿透】：如果是之前存入的空对象标记，直接返回空
        if (val == "[EMPTY_DATA]") return ""; 
        std::cout << "[Cache] ⚡ 命中缓存: " << key << " -> " << val << std::endl;
        return val; 
    }

    // --- 步骤 2：【防击穿】--- 
    // 使用 Double-Check Locking (双重检查锁)，保证只有一个线程去查数据库
    std::shared_ptr<std::mutex> key_lock;
    {
        std::lock_guard<std::mutex> global_lock(loader_mtx_);
        if (key_locks_.find(key) == key_locks_.end()) {
            key_locks_[key] = std::make_shared<std::mutex>();
        }
        key_lock = key_locks_[key];
    }

    // 只有一个线程能拿到这个 key 的锁
    std::lock_guard<std::mutex> lock(*key_lock);

    // --- 步骤 3：Double Check ---
    // 再次查缓存，可能刚才排队的时候，前一个线程已经查完库并写回缓存了
    if (cache_.get(key, val)) {
        if (val == "[EMPTY_DATA]") return "";
        std::cout << "[Cache] 醒来发现别人已经查好了: " << val << std::endl;
        return val;
    }

    // --- 步骤 4：查后端 MySQL ---
    std::cout << "[DB] 🐌 缓存未命中，开始查库: " << key << "...\n";
    std::string db_result = g_mysql.query(key); // 修改为查 MySQL

    if (db_result.empty()) {
        // --- 步骤 5：【防穿透】 ---
        // 数据库也没有！存入一个短期过期的空对象，防止黑客疯狂查询不存在的 key 攻击 DB
        std::cout << "[Cache] 🛡️ 数据库也没数据，存入空对象防穿透" << std::endl;
        cache_.put(key, "[EMPTY_DATA]", 30); // 30秒短期有效
        return "";
    } else {
        // --- 步骤 6：【防雪崩】 ---
        // 查到了！存入缓存，并给 TTL 增加随机抖动，防止大量缓存同时失效
        int safe_ttl = 3600 + get_random_jitter();
        cache_.put(key, db_result, safe_ttl);
        return db_result;
    }
}