#pragma once
#include <vector>
#include <memory>
#include "ICache.h"
#include <functional>
#include "ARC.h"

template <typename K, typename V>
class ShardedCache : public ICache<K, V> {
private:
    size_t shard_count_;
    std::vector<std::unique_ptr<ICache<K, V>>> shards_;
    std::hash<K> hasher_;

    size_t getShardIndex(const K& key) const {
        return hasher_(key) % shard_count_;
    }
public:
    // 【修改点】：增加 cb 参数，并传给底层的 ARCCache
    ShardedCache(size_t shard_count, int total_capacity, std::function<void(const K&, const V&)> cb = nullptr) 
        : shard_count_(shard_count) {
        
        int capacity_per_shard = (total_capacity / shard_count);
        if(capacity_per_shard == 0) capacity_per_shard = 1;

        for(size_t i = 0; i < shard_count; i++) {
            // 把回调函数 cb 喂给 ARC，这样底层触发淘汰时，高层就能知道！
            shards_.emplace_back(std::make_unique<ARCCache<K, V>>(capacity_per_shard, cb));
        }
    }

    bool get(const K& key, V& out_value) override {
        size_t index = getShardIndex(key);
        return shards_[index]->get(key, out_value);
    }

    void put(const K& key, const V& value, int ttl_seconds = 0) override {
        size_t index = getShardIndex(key);
        shards_[index]->put(key, value, ttl_seconds);          
    }
};