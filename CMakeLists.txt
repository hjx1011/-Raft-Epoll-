cmake_minimum_required(VERSION 3.10)
project(UltimateKVGateway)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 强制添加 pthread 库 (多线程必备)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")

# ==========================================
# 1. 设置头文件搜索路径 (这样代码里 include 就不用写很长的相对路径了)
# ==========================================
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/network
    ${CMAKE_SOURCE_DIR}/src/client
    ${CMAKE_SOURCE_DIR}/third_party/KamaCache/include
    ${CMAKE_SOURCE_DIR}/third_party/KVstorageBaseRaft/include
)

# ==========================================
# 2. 收集源文件
# ==========================================
# 缓存模块源文件
set(CACHE_SOURCES 
    third_party/KamaCache/src/CacheManager.cpp
)

# Raft 模块源文件 (注意：不包含 Raft 的 main.cpp)
set(RAFT_SOURCES 
    third_party/KVstorageBaseRaft/src/RaftNode.cpp
    third_party/KVstorageBaseRaft/src/RaftRpc.cpp
)

# 网关网络层源文件
set(GATEWAY_SOURCES 
    src/network/EpollServer.cpp
)

# ==========================================
# 3. 编译目标 1：Raft 节点程序 (底层数据库)
# ==========================================
add_executable(raft_node 
    third_party/KVstorageBaseRaft/src/main.cpp 
    ${RAFT_SOURCES}
)

# ==========================================
# 4. 编译目标 2：API 网关程序 (核心服务)
# ==========================================
add_executable(gateway 
    src/GatewayMain.cpp 
    ${GATEWAY_SOURCES} 
    ${CACHE_SOURCES} 
    ${RAFT_SOURCES}
)

# ==========================================
# 5. 编译目标 3：并发防御测试 (可选)
# ==========================================
add_executable(test
    tests/ConcurrencyTest.cpp
    ${CACHE_SOURCES}
    ${RAFT_SOURCES}
)
# ==========================================
# 6. 链接外部库 (这是报错的关键，必须加上)
# ==========================================

# 给 Raft 节点链接线程库
target_link_libraries(raft_node pthread)

# 给网关链接：MySQL 库、线程库
target_link_libraries(gateway mysqlcppconn pthread)

# 给并发测试链接：MySQL 库、线程库
target_link_libraries(test mysqlcppconn pthread)