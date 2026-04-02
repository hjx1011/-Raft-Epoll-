基于 C++14 实现的工业级分布式高并发 API 网关系统。集成了**高性能异步网络模型**、**自适应缓存淘汰算法**以及**分布式强一致性共识协议**。

## 🌟 核心特性 (Features)

- **高性能网络层**: 基于 `epoll` (ET模式) + `Reactor` 架构，结合 C++11 线程池处理海量并发连接。
- **高吞吐缓存层**: 自研 16 分片锁的 `ARC` (Adaptive Replacement Cache) 淘汰算法，极大地提高了缓存命中率并降低了锁竞争。
- **企业级高可用保护**: 完整实现了 `Cache-Aside` 旁路缓存策略，内置防御**缓存穿透（空对象标记）**、**缓存击穿（双重检查锁）**、**缓存雪崩（TTL抖动）**。
- **分布式强一致性**: 纯手工实现 `Raft` 分布式共识协议集群，提供高可靠的元数据存储。
- **原生分布式锁**: 抛弃传统 Redis 锁方案，利用 Raft 集群强一致性实现底层分布式锁，保障并发环境下 MySQL 数据库的绝对写入安全。

## 🏗️ 架构设计 (Architecture)

系统采用经典的四层工业级解耦架构：读请求通过网关 ARC 缓存秒回；写请求经 Raft 抢占分布式锁后，安全写入 MySQL 持久化。

```text[ 外部客户端 (HTTP) ]
             │
   ┌─────────▼─────────────────────────────┐
   │  EpollServer (Reactor 异步网络层)      │ 
   └─────────┬─────────────────────────────┘
             ▼
   ┌───────────────────────────────────────┐
   │  CacheManager (Cache-Aside 核心逻辑)   │ 
   └─────────┬──────────────────┬──────────┘
             │                  │
      [ 1. 查缓存 ][ 2. 缓存未命中 / 并发写 ]
             │                  │
   ┌─────────▼─────────┐  ┌─────▼──────────────┐
   │  ShardARC-Cache   │  │   RaftKVClient     │ ---> [ MySQL (数据持久化) ]
   │ (本地高并发缓存)   │  │  (Raft 抢锁控制器)  │
   └───────────────────┘  └─────┬──────────────┘
                                │ (自定义 TCP RPC)
             ┌──────────────────┼──────────────────┐
             ▼                  ▼                  ▼
      +------------+      +------------+      +------------+
      | RaftNode 0 | <──> | RaftNode 1 | <──> | RaftNode 2 |
      |  (Leader)  |      | (Follower) |      | (Follower) |
      +------------+      +------------+      +------------+
```

## 🛠️ 构建与安装 (Build & Run)

### 1. 环境依赖
* Linux 操作系统 (Ubuntu/CentOS)
* CMake 3.10+
* GCC 编译器 (支持 C++14)
* MySQL Connector/C++ (`sudo apt-get install libmysqlcppconn-dev`)

### 2. 编译项目
```bash
git clone https://github.com/YourUsername/YourProjectName.git
cd YourProjectName
mkdir build && cd build
cmake ..
make
```

### 3. 运行分布式集群测试
需要打开多个终端模拟分布式环境：

**启动底层的 Raft 共识集群：**
```bash
# 终端 1
./raft_node 0
# 终端 2
./raft_node 1
# 终端 3
./raft_node 2
```

**启动 API 网关主程序：**
```bash
# 终端 4
./gateway
```

**发送测试请求：**
```bash
# 终端 5: 写入数据 (将触发 Raft 抢锁与 MySQL 写入)
curl -X POST "http://127.0.0.1:8080/set?key=hero&val=batman"

# 读取数据 (将触发 Cache 极速秒回)
curl "http://127.0.0.1:8080/get?key=hero"
```

## 📂 目录结构 (Directory Structure)
* `src/` : 网关主程序、Epoll Reactor 网络层实现。
* `third_party/KamaCache/` : 自研分片 ARC 缓存模块与防御策略。
* `third_party/KVstorageBaseRaft/` : 自研 Raft 分布式协议实现与 RPC 框架。
* `tests/` : 并发防御与系统稳定性压测代码。