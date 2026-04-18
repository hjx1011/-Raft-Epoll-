# Ultimate-KV-Gateway：工业级分布式高并发 API 网关系统

## 📖 项目简介

本项目是一个基于 C++14 开发的高性能、分布式 API 网关系统。它不仅实现了高性能的 **Epoll-Reactor** 异步网络引擎，还自研了具备 **ARC (Adaptive Replacement Cache)** 淘汰算法的分片缓存层。

最核心的特色在于：系统集成了 **Raft 分布式共识协议**，利用 Raft 的强一致性实现了**原生分布式锁**，彻底解决了高并发场景下双写一致性与 MySQL 写入安全问题。

-----

## 🌟 核心特性 (Features)

### 1\. 高性能异步网络层

  * **Reactor 模型**: 基于 `epoll` 边缘触发 (ET) 模式实现，结合 `ONESHOT` 事件确保多线程下连接处理的安全性。
  * **智能线程池**: 使用自定义线程池处理业务逻辑，实现网络 I/O 与业务逻辑的彻底解耦。

### 2\. 强一致性分布式协议 (Raft)

  * **完整 Raft 实现**: 纯手工实现 Leader 选举、日志复制、快照安装 (Snapshot) 及持久化恢复逻辑。
  * **gRPC 通信**: 节点间采用 gRPC 进行高效的二进制数据同步，并内置了连接池优化。
  * **分布式锁服务**: 基于 Raft 状态机实现 `attemptLock` 和 `releaseLock` 接口，为后端 MySQL 提供独占式写入保护。

### 3\. 三级高可用缓存策略

  * **ARC 算法**: 实现自适应替换缓存 (ARC)，自动平衡 LRU 与 LFU 逻辑，提升热点数据命中率。
  * **分片锁设计**: 采用 16 分片 (Sharding) 技术降低全局锁竞争，大幅提升并发吞吐量。
  * **企业级防御**:
      * **防穿透**: 存入空对象标记并设置短期 TTL。
      * **防击穿**: 结合分段锁 (Lock Striping) 与 Double-Check 机制。
      * **防雪崩**: 引入 TTL 随机抖动 (Jitter)，避免缓存同时失效。

-----

## 🏗️ 系统架构 (Architecture)

1.  **读路径 (Read Path)**: Client → Epoll Server → ARC Cache (Hit) → Response。
2.  **写路径 (Write Path)**: Client → Raft Cluster (获取分布式锁) → MySQL (写入) → 更新缓存 → 释放锁。

-----

## 📂 目录结构 (Directory Structure)

```text
.
├── src/                # 网关核心逻辑与 Reactor 网络引擎
├── third_party/
│   ├── KamaCache/      # 自研分布式缓存模块 (ARC/LRU/LFU/分片锁)
│   └── KVstorageBaseRaft/ # Raft 共识协议实现与状态机
├── proto/              # gRPC 服务定义
├── tests/              # 压力测试与并发场景测试工具
└── start_cluster.sh    # 一键部署与集群启动脚本
```

-----

## 🛠️ 构建与运行 (Getting Started)

### 环境要求

  * Ubuntu 18.04+
  * CMake 3.10+
  * gRPC & Protobuf
  * MySQL Connector/C++

### 编译与启动

```bash
# 1. 克隆并编译
git clone <your-repo-url>
mkdir build && cd build
cmake .. && make -j4

# 2. 一键启动集群 (3个Raft节点 + 1个网关)
bash ../start_cluster.sh
```

### 接口测试

```bash
# 写入数据 (触发 Raft 抢锁)
curl -X POST "http://127.0.0.1:8081/set?key=user_1&val=active"

# 读取数据 (缓存加速)
curl "http://127.0.0.1:8081/get?key=user_1"
```

-----

## 📊 性能压测 (Benchmark)

使用项目内置的 `benchmark` 工具进行离散键值对压测：

```bash
./build/benchmark <线程数> <每个线程请求数> <read/write>
```

在标准 Linux 环境下，系统可展示出卓越的 QPS 表现，特别是在开启分片锁后的缓存命中性能。

-----

## 📜 核心模块说明

  * **CacheManager**: 封装了 `Cache-Aside` 模式，统一管理 Raft 锁与数据库写入。
  * **RaftNode**: 负责维护任期 (Term)、心跳 (Heartbeat) 及日志的一致性。
  * **EpollServer**: 负责非阻塞 I/O 的读写分发，支持千万级长连接并发潜力。