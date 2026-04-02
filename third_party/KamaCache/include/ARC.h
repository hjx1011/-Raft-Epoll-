#pragma once
#include <iostream> 
#include <unordered_map>
#include <list>
#include <algorithm>
#include <mutex>
#include "ICache.h"
#include <functional>
#include <chrono>

template <typename K, typename V>
class ARCCache : public ICache<K,V> {
private:
    enum ListType { IN_T1, IN_T2, IN_B1, IN_B2 };

    struct Node{
        K key;
        V value;
        ListType type;
        typename std::list<K>::iterator ptr;
        std::chrono::steady_clock::time_point expire_time; // 过期时间点
    };

    // T1: 最近访问区 
    // T2: 频繁访问区
    // B1: T1 的淘汰历史
    // B2: T2 的淘汰历史
    std::list<K> T1, T2 ,B1, B2;
    std::unordered_map<K,Node> map;
    int c;
    int p=0;
    std::function<void(const K&,const V&)> callback;
    std::mutex mtx;
    // 淘汰引擎：当真实缓存 (T1+T2) 满了时，决定淘汰谁，并把它降级为幽灵
    void replace(bool hit_in_B2) {
        //(hit_in_B2 && T1.size() == p) 这行代码在数学上是冗余的，保留仅为忠于原论文
        if(!T1.empty() && (T1.size() > p || (hit_in_B2 && T1.size() == p))) {
            K old_key = T1.back();
            T1.pop_back();
            // 降级为幽灵缓存，放入 B1 头部
            if(callback) {
                callback(old_key, map[old_key].value);
            }
            B1.push_front(old_key);
            //同步更新哈希表中的状态和迭代器
            map[old_key].type = IN_B1;
            map[old_key].ptr = B1.begin();
        } else {
            K old_key = T2.back();
            T2.pop_back();

            if (callback) { 
                callback(old_key, map[old_key].value); 
            }
            //降级为幽灵缓存，放入 B2 头部
            B2.push_front(old_key);
            //同步更新哈希表中的状态和迭代器
            map[old_key].type = IN_B2;
            map[old_key].ptr = B2.begin();
        }
    }

    bool clean_if_expired(typename std::unordered_map<K,Node>::iterator it) {
        auto now = std::chrono::steady_clock::now();

        if(now < it->second.expire_time) {
            return false;
        }

        Node& node = it->second;
        if(node.type == IN_T1) T1.erase(node.ptr);
        else if(node.type == IN_T2) T2.erase(node.ptr);
        else if(node.type == IN_B1) B1.erase(node.ptr);
        else if(node.type == IN_B2) B2.erase(node.ptr);

        if(callback && (node.type ==IN_T1 || node.type == IN_T2)) {
            callback(node.key, node.value);
        }
        map.erase(it);
        return true;
    }
public:
    // 【核心改动 3】：构造函数支持传入回调函数，默认值为 nullptr（不传就不回调）
    ARCCache(int capacity, std::function<void(const K&,const V&)> cd = nullptr) :c(capacity), p(0), callback(cd) {}    

    bool get(const K& key,V& out_value) override {
        std::lock_guard<std::mutex> lock(mtx);

        auto it = map.find(key);
        if(it == map.end()) return false;

        if(clean_if_expired(it)) {
            return false;
        }

        Node& node = it -> second;
        if(node.type == IN_T1 || node.type == IN_T2) {
            if(node.type == IN_T1) T1.erase(node.ptr);
            else T2.erase(node.ptr);
            
            // 既然被再次访问了，说明是高频数据，放入 T2 头部
            T2.push_front(key);
            node.type = IN_T2;
            node.ptr = T2.begin();

            out_value = node.value;
            return true; 
        }

        // 如果在幽灵缓存中（B1 或 B2），说明真实数据早就被踢了，视为 Cache Miss
        return false;
    }

    void put(const K& key,const V& value, int ttl_seconds = 0) override {
        std::lock_guard<std::mutex> lock(mtx);
        if(c == 0) return;
        
        auto expire_time = (ttl_seconds > 0) 
            ? std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds)
            : std::chrono::steady_clock::time_point::max();

        auto it = map.find(key);

        if(it != map.end()) {
            if(clean_if_expired(it)) {
                // 如果是尸体，已经被删了，强行把迭代器置为 end()，直接走下面的【情况 B：全新插入】逻辑
                it = map.end();
            }
        }
        // 【情况 A】：Key 已经在系统中（在 4 条链表的某一条里）
        if(it != map.end()) {
            Node& node = it->second;
            
            node.expire_time = expire_time;
            // 命中真实缓存（T1 或 T2）
            if(node.type == IN_T1 || node.type == IN_T2) {
                node.value = value;
                if(node.type == IN_T1) T1.erase(node.ptr);
                else T2.erase(node.ptr);

                T2.push_front(key);
                node.type = IN_T2;
                node.ptr = T2.begin();
                return;
            }
            //命中 B1
            if(node.type == IN_B1) {
                int delta = (B1.size() >= B2.size()) ? 1 :(B2.size() / B1.size());
                p = std::min(c, p + delta);

                replace(false);

                B1.erase(node.ptr);
                T2.push_front(key);
                node.value = value;
                node.type = IN_T2;
                node.ptr = T2.begin();
                return;
            }
            //命中 B2
            if(node.type == IN_B2) {
                int delta = (B2.size() >= B1.size()) ? 1 :(B1.size()/B2.size());
                p = std::max(0,p-delta);

                replace(true);

                B2.erase(node.ptr);
                T2.push_front(key);
                node.value = value;
                node.type = IN_T2;
                node.ptr = T2.begin();
                return;
            }
        }

        // 【情况 B】：全新的 Key，引发 Cache Miss，准备放入 T1
        // 限制总幽灵缓存的数量绝不超过 c
        if(T1.size() + B1.size() == c) {
            // 如果真实数据 T1 还没占满 c，说明有幽灵 B1 存在
            if(T1.size() < c) {
                map.erase(B1.back());// 彻底从系统中删除最老的幽灵
                B1.pop_back();
                replace(false);
            } else {
                // 如果 T1 自己就把容量占满了，连产生幽灵的资格都没有，直接彻底干掉最老的 T1 数据
                K old_key = T1.back();
                if (callback) {
                    callback(old_key, map[old_key].value);
                }
                map.erase(old_key);
                T1.pop_back();
            }
        } 
         // 近期区没满，但四条链表总和已经到了最大限制 (2 * c)
        else if(T1.size() + B1.size() < c && T1.size() + T2.size() + B1.size() + B2.size() >= c) {
            if (T1.size() + T2.size() + B1.size() + B2.size() == 2 * c) {
                map.erase(B2.back()); // 四条链表全满了，删掉最老的 B2 幽灵
                B2.pop_back();
            }
            replace(false);
        } 

        T1.push_front(key);
        map[key] = {key, value, IN_T1, T1.begin(), expire_time};
    }
};