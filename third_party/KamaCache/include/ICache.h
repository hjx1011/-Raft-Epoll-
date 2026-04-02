#pragma once
#include <chrono>
template <typename K,typename V>
class ICache {
public:
    virtual ~ICache() = default;

    //[[nodiscard]]强制检查返回值
    [[nodiscard]] virtual bool get(const K& key, V& out_value) = 0;
    virtual void put(const K& key, const V& value, int ttl_second = 0 ) = 0;
};