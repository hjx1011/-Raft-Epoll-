#pragma once
#include <unordered_map>
#include <list>
#include <mutex>
#include "ICache.h"
#include <functional>
template <typename K,typename V>
class LFUCache : public ICache <K,V> {
private:
    // 1. 缓存节点，现在必须多存一个 freq（频率）
    struct Node {
        K key;
        V value;
        int freq;
    };

    int capacity_;
    int min_freq_; // 核心变量：记录当前最小频率，用于 $O(1)$ 淘汰
    std::mutex mtx;
    std::function<void(const K&,const V&)> callback;
    // 2. 频率表 (桶)：频率 freq -> 属于该频率的双向链表
    // 链表头部是最近访问的，尾部是最久未访问的
    std::unordered_map<int, std::list<Node>> freq_table_;

    // 3. 键值表：Key -> 节点在某个链表中的位置(迭代器)
    std::unordered_map<K, typename std::list<Node>::iterator> key_table_;

    // 内部辅助函数：将节点频率 +1，并移动到新的频率桶中
    void updateFreq(typename std::list<Node>::iterator it) {
        K key = it->key;
        V value = it->value;
        int freq = it->freq;

        // 1. 从当前的频率桶中删除该节点
        freq_table_[freq].erase(it);

        // 2. 如果当前频率桶空了，而且这个频率恰好是 min_freq_，那最小频率就得被动升级了！
        if (freq_table_[freq].empty() && min_freq_ == freq) {
            min_freq_++; 
            freq_table_.erase(freq); // 释放空桶的内存(工程好习惯)
        }

        // 3. 频率 +1，放入下一个频率桶的最前面
        int new_freq = freq + 1;
        freq_table_[new_freq].push_front({key, value, new_freq});
        
        // 4. 更新 key_table_ 中记录的指针
        key_table_[key] = freq_table_[new_freq].begin();
    }

public:
    LFUCache(int capacity, std::function<void(const K&, const V&)> cb = nullptr) : capacity_(capacity), min_freq_(0), callback(cb) {}
    
    bool get(const K& key, V& out_value) override{
        std::lock_guard<std::mutex> lock(mtx);
        if (capacity_ == 0) return false;

        auto it = key_table_.find(key);
        if (it == key_table_.end()) return false;

        // 找到了，获取迭代器
        auto node_it = it->second;
        out_value = node_it->value;

        // 核心：升级频率！
        updateFreq(node_it);

        return true;
    }
    
    void put(const K& key,const V& value) override {
        std::lock_guard<std::mutex> lock(mtx);
        if (capacity_ == 0) return;

        auto it = key_table_.find(key);
        if (it != key_table_.end()) {
            // 如果已存在：更新值，然后升级频率
            auto node_it = it->second;
            node_it->value = value;
            updateFreq(node_it);
            return;
        }

        // 如果不存在，是新节点：
        // 1. 检查是否满了，满了要淘汰
        if (key_table_.size() == capacity_) {
            // 找到最小频率的桶，淘汰里面的末尾节点（最久未访问）
            auto& min_list = freq_table_[min_freq_];
            K old_key = min_list.back().key;
            V old_value = min_list.back().value;
            min_list.pop_back(); // 从链表中删除

            if(callback) {
                callback(old_key, old_value);
            }
            key_table_.erase(old_key); // 从哈希表中删除
            
            if (min_list.empty()) {
                freq_table_.erase(min_freq_); // 空桶清理
            }
        }

        // 2. 插入新节点：新节点的频率永远是 1
        min_freq_ = 1; // 既然有新元素进来，最小频率强制重置为 1
        freq_table_[1].push_front({key, value, 1});
        key_table_[key] = freq_table_[1].begin();
    }
};