#pragma once
#include "CacheManager.h"

// 对外暴露的压测接口
void run_concurrency_test(HighAvailableCacheManager* manager);