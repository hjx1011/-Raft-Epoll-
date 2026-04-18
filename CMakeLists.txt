cmake_minimum_required(VERSION 3.10)
project(UltimateKVGateway)

# ==========================================
# 1. 基础环境设置
# ==========================================
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 强制添加 pthread 库 (多线程必备)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")

# ==========================================
# 2. 查找 gRPC 和 Protobuf 依赖
# ==========================================
# 在 Ubuntu 上通常需要链接以下库
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

# 获取 gRPC 插件路径 (用于生成 C++ 代码)
find_program(GRPC_CPP_PLUGIN NAMES grpc_cpp_plugin)

# ==========================================
# 3. 自动编译 .proto 文件逻辑
# ==========================================
# 定义 proto 文件位置
set(PROTO_DIR "${CMAKE_SOURCE_DIR}/proto")
set(PROTO_FILE "${PROTO_DIR}/raft.proto")

# 定义生成的代码文件位置
set(GEN_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${GEN_DIR})

set(GEN_PROTO_SRC "${GEN_DIR}/raft.pb.cc")
set(GEN_PROTO_HDR "${GEN_DIR}/raft.pb.h")
set(GEN_GRPC_SRC "${GEN_DIR}/raft.grpc.pb.cc")
set(GEN_GRPC_HDR "${GEN_DIR}/raft.grpc.pb.h")

# 使用 add_custom_command，当 .proto 文件修改时自动重新生成代码
add_custom_command(
    OUTPUT "${GEN_PROTO_SRC}" "${GEN_PROTO_HDR}" "${GEN_GRPC_SRC}" "${GEN_GRPC_HDR}"
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    ARGS --grpc_out "${GEN_DIR}"
         --cpp_out "${GEN_DIR}"
         -I "${PROTO_DIR}"
         --plugin=protoc-gen-grpc="${GRPC_CPP_PLUGIN}"
         "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Generating gRPC/Protobuf code from ${PROTO_FILE}"
)

# ==========================================
# 4. 设置头文件搜索路径
# ==========================================
include_directories(
    ${GEN_DIR}                              # 必须包含生成的 .pb.h 目录
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/network
    ${CMAKE_SOURCE_DIR}/src/client
    ${CMAKE_SOURCE_DIR}/third_party/KamaCache/include
    ${CMAKE_SOURCE_DIR}/third_party/KVstorageBaseRaft/include
)

# ==========================================
# 5. 收集源文件
# ==========================================
# 缓存模块
set(CACHE_SOURCES 
    third_party/KamaCache/src/CacheManager.cpp
)

# Raft 模块 (加入生成的 gRPC 代码)
set(RAFT_SOURCES 
    third_party/KVstorageBaseRaft/src/RaftNode.cpp
    third_party/KVstorageBaseRaft/src/RaftRpc.cpp
    "${GEN_PROTO_SRC}"
    "${GEN_GRPC_SRC}"
)

# 网络网关模块
set(GATEWAY_SOURCES 
    src/network/EpollServer.cpp
)

# ==========================================
# 6. 编译目标定义
# ==========================================

# 目标 1：Raft 节点
add_executable(raft_node 
    third_party/KVstorageBaseRaft/src/main.cpp 
    ${RAFT_SOURCES}
)

# 目标 2：API 网关
add_executable(gateway 
    src/GatewayMain.cpp 
    ${GATEWAY_SOURCES} 
    ${CACHE_SOURCES} 
    ${RAFT_SOURCES}
)

# 目标 3：并发测试
add_executable(test
    tests/ConcurrencyTest.cpp
    ${CACHE_SOURCES} 
    ${RAFT_SOURCES}
)

# ==========================================
# 7. 链接外部库 (终极优化版)
# ==========================================

# 定义公共库变量，简化代码
set(COMMON_LIBS 
    gRPC::grpc++ 
    protobuf::libprotobuf 
    pthread 
    spdlog 
    fmt
)

target_link_libraries(raft_node ${COMMON_LIBS})

target_link_libraries(gateway mysqlcppconn ${COMMON_LIBS})

target_link_libraries(test mysqlcppconn ${COMMON_LIBS})


# 编译目标 4：压测工具
add_executable(benchmark tests/benchmark.cpp)
target_link_libraries(benchmark pthread)