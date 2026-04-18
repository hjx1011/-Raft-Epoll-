#include "CacheManager.h"
#include "MySQLClient.h"
#include <iostream>
#include <random>
#include <thread>
#include "spdlog/spdlog.h"

// 全局 MySQL 客户端
MySQLClient g_mysql;

auto global_eviction_callback = [](const std::string& k, const std::string& v) {
    spdlog::debug("🗑️ [Cache 淘汰] Key: {}, Val: {}", k, v);
};

HighAvailableCacheManager::HighAvailableCacheManager() 
    : cache_(16, 10000, global_eviction_callback) {} // 增加缓存容量到 10000，适应高并发

// 【防雪崩】：获取 0-60 秒的随机抖动，防止缓存同时大面积失效
int HighAvailableCacheManager::get_random_jitter() {
    // 【高并发优化】：使用 thread_local 避免多线程竞争同一个随机数生成器
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 60);
    return dis(gen);
}

// 【分布式一致性写】：Raft 锁保护下的 MySQL 写入
bool HighAvailableCacheManager::write_data(const std::string& key, const std::string& value) {
    // 加上线程ID防止不同线程使用相同的 clientId 导致锁误判
    std::string clientId = "Gateway_Node_1_" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    
    spdlog::debug("[Step 1] 准备向 Raft 集群申请分布式锁: {}", key);
    
    // 1. 抢分布式锁
    if (raft_client_.tryLock(key, clientId)) {
        spdlog::debug("[Step 2] 🟢 抢锁成功！准备写入 MySQL...");
        
        // 2. 写入 MySQL
        if (g_mysql.upsert(key, value)) {
            spdlog::debug("[Step 3] 🟢 MySQL 写入成功！更新缓存...");
            cache_.put(key, value, 3600 + get_random_jitter());
            raft_client_.unlock(key);
            return true;
        } else {
            // MySQL 失败分支
            spdlog::error("[Step 3] 🔴 MySQL 写入失败！请检查：1.密码是否正确 2.kv_project库和kv_table表是否已创建！");
            raft_client_.unlock(key);
        }
    } else {
        // Raft 失败分支
        spdlog::error("[Step 1] 🔴 获取 Raft 锁失败！请检查 3 个 Raft 节点是否都在运行并选出了 Leader！");
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
        spdlog::debug("[Cache] ⚡ 命中缓存: {} -> {}", key, val);
        return val; 
    }

    // --- 步骤 2：【防击穿】--- 
    // 【高并发优化】：使用分段锁 (Lock Striping) 替代全局锁，大幅提升高并发下的吞吐量
    size_t stripe_idx = get_stripe_index(key);
    std::lock_guard<std::mutex> lock(stripe_locks_[stripe_idx]);

    // --- 步骤 3：Double Check ---
    // 再次查缓存，可能刚才排队的时候，前一个线程已经查完库并写回缓存了
    if (cache_.get(key, val)) {
        if (val == "[EMPTY_DATA]") return "";
        spdlog::debug("[Cache] 醒来发现别人已经查好了: {}", val);
        return val;
    }

    // --- 步骤 4：查后端 MySQL ---
    spdlog::debug("[DB] 🐌 缓存未命中，开始查库: {}...", key);
    std::string db_result = g_mysql.query(key); // 修改为查 MySQL

    if (db_result.empty()) {
        // --- 步骤 5：【防穿透】 ---
        // 数据库也没有！存入一个短期过期的空对象，防止黑客疯狂查询不存在的 key 攻击 DB
        spdlog::debug("[Cache] 🛡️ 数据库也没数据，存入空对象防穿透");
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
