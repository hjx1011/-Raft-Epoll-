#pragma once
#include <unordered_map>
#include <list>
#include <mutex> 
#include "ICache.h"
#include <functional>
template <typename K,typename V>
class LRUCache : public ICache<K,V> {
private:
    // 定义双向链表中存储的节点类型，必须存 Key，因为淘汰末尾节点时，需要根据 Key 去删哈希表
    struct CacheNode {
        K key;
        V value;
    };

    int capacity_;
    std::list<CacheNode> cache_list_; // 双向链表，存放具体数据。链表头表示最近使用，链表尾表示最久未使用
    std::mutex mtx;
    std::function<void(const K&,const V&)> callback;
    // 哈希表：Key 映射到 链表节点的迭代器(指针)
    std::unordered_map<K, typename std::list<CacheNode>::iterator> cache_map_;

public:
    LRUCache(int capacity, std::function<void(const K&, const V&)> cb = nullptr) : capacity_(capacity),callback(cb) {}
    
    bool get(const K& key, V& out_value) override {
        std::lock_guard<std::mutex> lock(mtx);

        auto it = cache_map_.find(key);
        
        if(it == cache_map_.end()) return false;

        cache_list_.splice(cache_list_.begin(),cache_list_, it->second);
        out_value = it->second->value;
        return true;
    }
    
    void put(const K& key,const V& value) override {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = cache_map_.find(key);
        if(it != cache_map_.end()) {
            it -> second ->value = value;
            cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
            return;
        }

        if(cache_list_.size() == capacity_) {
            K old_key = cache_list_.back().key;
            V old_value = cache_list_.back().value;
            cache_list_.pop_back();

            if(callback) {
                callback(old_key, old_value);
            }
            cache_map_.erase(old_key);
        }
        cache_list_.push_front({key,value});
        cache_map_[key] = cache_list_.begin();
    }
};