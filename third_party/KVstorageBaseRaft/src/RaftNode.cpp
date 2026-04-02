#include "RaftNode.h"
#include "RaftRpc.h" // 【重要新增】：引入 RPC 网络模块
#include <iostream>
#include <sstream>
#include <cstring>

// 构造函数初始化节点
RaftNode::RaftNode(int myId, int peers, std::shared_ptr<Persister> p) : me(myId), peerCount(peers), persister(p) {
    nextIndex.resize(peers, 1);
    matchIndex.resize(peers, 0);
    
    std::string data = persister->ReadRaftState();
    if (data.empty()) {
        // 情况 A：第一次启动，初始化默认状态
        currentTerm = 0;
        votedFor = -1;
        logs.push_back(LogEntry(0, "InitLog"));
        
        // 第一次初始化后，立刻存一次盘
        persist(); 
        std::cout << "节点 " << me << " 首次启动，初始化成功。\n";
    } else {
        // 情况 B：崩溃重启，调用恢复逻辑
        readPersist(data);
        std::cout << "节点 " << me << " 崩溃重启成功，已恢复 " << logs.size() << " 条日志。\n";
    }

    std::random_device rd;
    randomGenerator.seed(rd());
    lastHeartBeat = std::chrono::system_clock::now();
    tickerThread = std::thread(&RaftNode::ticker, this);
}

void RaftNode::ticker() {
    while (running) {
        // 每次循环先睡一小会儿 (比如 10 毫秒)，防止 CPU 100% 飙升
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        bool shouldStartElection = false;

        mtx.lock(); // 巨重要！检查状态必须加锁！
        
        if (state != NodeState::LEADER) {

            // 看看现在几点了
            auto now = std::chrono::system_clock::now();
            
            // 算一下：距离上一次收到心跳，过去了多少毫秒？
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHeartBeat).count();
            // 生成一个 150 ~ 300 之间的随机数，作为今天的炸弹超时时间
            std::uniform_int_distribution<int> dist(150, 300);
            int timeout = dist(randomGenerator);
                // 核心判断：如果过去的时间，超过了随机指定的超时时间，炸弹爆炸！
            if (duration > timeout) {
                shouldStartElection = true; // 标记一下，我们要开始选举了
                lastHeartBeat = std::chrono::system_clock::now(); 
            }
        }
        mtx.unlock(); // 解锁
        
        if (shouldStartElection) {
            std::cout << "节点超时！大哥死了，我要发起选举！" << std::endl;
            // 变成候选人，开始选举 (我们下一课写这个函数)
            startElection(); 
        }
        
    }
}

void RaftNode::startElection() {
    mtx.lock(); // 注意：这里手动加锁，因为后面要解锁
    state = NodeState::CANDIDATE;
    currentTerm++;
    votedFor = me;
    currentVotes = 1; // 投给自己一票
    persist(); // 【重要新增】：任期和投票变了，立刻持久化
    
    // 准备发送的参数
    RequestVoteArgs args;
    args.term = currentTerm;
    args.candidateId = me;
    
    std::cout << "[竞选] 节点 " << me << " 开始竞选！当前任期: " << currentTerm << std::endl;
    mtx.unlock(); // 准备发 RPC 前必须解锁！否则会死锁！

    // 并发向其他节点拉票
    for (int i = 0; i < peerCount; i++) {
        if (i == me) continue;

        std::thread([this, i, args]() {
            // 调用真实的拉票逻辑
            bool granted = sendRequestVote(i, args);
            
            if (granted) {
                std::lock_guard<std::mutex> lock(mtx); 
                currentVotes++;
                
                // 如果票数过半，且我还是候选人（没被别人抢先），我就当选！
                if (state == NodeState::CANDIDATE && currentVotes > peerCount / 2) {
                    becomeLeader();
                }
            }
        }).detach(); 
    }
}

// 模拟发送拉票 RPC 请求
bool RaftNode::sendRequestVote(int targetId, RequestVoteArgs args) {
    // 模拟网络延迟 (10~20ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(15)); 
    
    bool success = false;
    // 【修改】：通过真实的 TCP RPC 调用目标节点
    RequestVoteReply reply = RaftRpc::callRequestVote(peerPorts[targetId], args, success);

    if(!success) return false;
    
    // 如果对方任期比我高，我立刻退选！
    if (reply.term > args.term) {
        std::lock_guard<std::mutex> lock(mtx);
        currentTerm = reply.term;
        state = NodeState::FOLLOWER;
        votedFor = -1;
        persist(); // 【重要新增】：发现更高任期，重置状态并持久化
        return false;
    }

    return reply.voteGranted;
}

// 当收到别人的拉票请求时，调用这个函数
RequestVoteReply RaftNode::handleRequestVote(RequestVoteArgs args) {
    std::lock_guard<std::mutex> lock(mtx);
    RequestVoteReply reply;
    reply.voteGranted = false; // 默认先拒绝

    // 1. 如果你（候选人）的任期还没我高，想当大哥？没门！直接拒绝。
    if (args.term < currentTerm) {
        reply.term = currentTerm;
        return reply;
    }

    // 2. 如果发现对方任期比我高，我立刻认怂，变成 Follower，并更新我的任期
    if (args.term > currentTerm) {
        currentTerm = args.term;
        state = NodeState::FOLLOWER;
        votedFor = -1; // 新任期，我还没投过票
        persist(); // 【重要新增】：任期更新，持久化
    }

    // 3. 决定是否投票：
    // 如果我还没投过票 (votedFor == -1)，或者我已经投给这个人了 (votedFor == args.candidateId)
    if (votedFor == -1 || votedFor == args.candidateId) {
        votedFor = args.candidateId; // 记录下我把票投给了谁
        persist(); // 【重要新增】：决定投票给某人，必须持久化
        reply.voteGranted = true;    // 同意投票！
        
        // 巨重要：既然我投票给了别人，说明集群里有活跃的选举，我要重置我的炸弹！
        lastHeartBeat = std::chrono::system_clock::now(); 
        std::cout << "节点 " << me << " 同意把票投给节点 " << args.candidateId << "\n";
    } else {
        std::cout << "节点 " << me << " 拒绝投票给节点 " << args.candidateId << " (我已经投给 " << votedFor << " 了)\n";
    }

    reply.term = currentTerm;
    return reply;
}

void RaftNode::becomeLeader() {
    if (state != NodeState::CANDIDATE) return; // 防止重复当选
    
    state = NodeState::LEADER;
    std::cout << "⭐⭐⭐ [当选] 节点 " << me << " 正式成为 Leader！任期: " << currentTerm << " ⭐⭐⭐\n";
    
    // 大哥上任的第一件事，就是初始化对小弟们的进度追踪
    for (int i = 0; i < peerCount; i++) {
        // 假设所有小弟的进度都跟大哥一样，下一个要发给他们的日志索引是大哥当前日志的下一条
        nextIndex[i] = getLastLogIndex() + 1; // 必须是真实的最高索引 + 1！
        matchIndex[i] = 0; 
    }

    startHeartbeatTimer();
}

void RaftNode::startHeartbeatTimer() {
    // 开启一个心跳线程
    std::thread([this]() {
        while (state == NodeState::LEADER && running) {
            // 大哥每隔 50ms 发一次，必须比 150ms 的炸弹快得多！
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            
            sendHeartbeats();
        }
    }).detach();
}

void RaftNode::sendHeartbeats() {
    for (int i = 0; i < peerCount; i++) {
        if (i == me) continue;

        // 并发给每个小弟发包
        std::thread([this, i]() {
            AppendEntriesArgs args;
            bool needInstallSnapshot = false;
            InstallSnapshotArgs snapArgs;

            {
                std::lock_guard<std::mutex> lock(mtx);
                if (state != NodeState::LEADER) return;
                
                int prevLogIndex = nextIndex[i] - 1;
                
                // 【终极判断】：如果小弟需要的日志已经被我删了，准备发送快照！
                if (prevLogIndex < lastIncludedIndex) {
                    needInstallSnapshot = true;
                    snapArgs.term = currentTerm;
                    snapArgs.leaderId = me;
                    snapArgs.lastIncludedIndex = lastIncludedIndex;
                    snapArgs.lastIncludedTerm = lastIncludedTerm;
                    snapArgs.data = persister->ReadSnapshot(); // 从硬盘读出快照数据
                } else {
                    // 正常的 AppendEntries 逻辑
                    args.term = currentTerm;
                    args.leaderId = me;
                    args.prevLogIndex = prevLogIndex;
                    args.prevLogTerm = (prevLogIndex == lastIncludedIndex) ? lastIncludedTerm : logs[getVectorIndex(prevLogIndex)].term;
                    args.leaderCommit = commitIndex;
                    for (int j = nextIndex[i]; j <= getLastLogIndex(); j++) {
                        args.entries.push_back(logs[getVectorIndex(j)]);
                    }
                }
            } // 解锁，准备进行耗时的网络 RPC 调用

            if (needInstallSnapshot) {
                // ================= 发送快照 =================
                bool snapSuccess = false;
                InstallSnapshotReply snapReply = RaftRpc::callInstallSnapshot(peerPorts[i], snapArgs, snapSuccess);
                if (!snapSuccess) return;

                std::lock_guard<std::mutex> lock(mtx);
                if (state != NodeState::LEADER) return;
                if (snapReply.term > currentTerm) {
                    currentTerm = snapReply.term;
                    state = NodeState::FOLLOWER;
                    votedFor = -1;
                    persist();
                    return;
                }
                // 快照发送成功！直接把小弟的进度拉满到快照的位置
                matchIndex[i] = snapArgs.lastIncludedIndex;
                nextIndex[i] = snapArgs.lastIncludedIndex + 1;
                
            } else {
                // ================= 发送正常心跳/日志 =================
                bool success = false;
                AppendEntriesReply reply = RaftRpc::callAppendEntries(peerPorts[i], args, success);
                if (!success) {
                    // std::cout << "⚠️ Leader " << me << " 发给小弟 " << i << " 的 RPC 超时或失败！\n";
                    return;
                }

                std::lock_guard<std::mutex> lock(mtx);
                if (state != NodeState::LEADER) return;
                if (reply.term > currentTerm) {
                    currentTerm = reply.term;
                    state = NodeState::FOLLOWER;
                    votedFor = -1;
                    persist();
                    return;
                } 
                if(reply.success) {
                    int newNext = args.prevLogIndex + args.entries.size() + 1;
                    if (newNext > nextIndex[i]) { // 只有进度真的往前走，才更新
                        nextIndex[i] = newNext;
                        matchIndex[i] = nextIndex[i] - 1;
                        updateCommitIndex();
                    }
                    // matchIndex[i] = args.prevLogIndex + args.entries.size();
                    // nextIndex[i] = matchIndex[i] + 1;
                    // updateCommitIndex(); 
                } else {
                    if (nextIndex[i] > 1) {
                        if (nextIndex[i] == args.prevLogIndex + 1) {
                            nextIndex[i]--;
                        }
                        // nextIndex[i]--;
                        std::cout << "Leader " << me << " 发现小弟 " << i 
                                  << " 日志对不上，下调其 nextIndex 到 " << nextIndex[i] << "，准备补课\n";
        }
                }
            }
        }).detach();
    }
}    

// 当网络层收到一个 AppendEntries 请求时，调用这个函数
AppendEntriesReply RaftNode::handleAppendEntries(AppendEntriesArgs args) {
    std::lock_guard<std::mutex> lock(mtx);
    AppendEntriesReply reply;
    reply.success = false;

    // 1. 如果对方任期比我小：直接拒绝，我不服！
    if (args.term < currentTerm) {
        reply.term = currentTerm;
        return reply;
    }

    bool needPersist = false; // 【重要新增】：标记是否需要存盘

    // 2. 只要对方任期 >= 我：承认他是大哥，重置我的炸弹！
    if (args.term > currentTerm) {
        currentTerm = args.term;
        votedFor = -1;
        needPersist = true; // 任期变了，标记需要存盘
    }
    
    state = NodeState::FOLLOWER; // 哪怕我之前是 Candidate，现在也得认怂
    lastHeartBeat = std::chrono::system_clock::now(); // 【重置炸弹】
    reply.term = currentTerm;

    // 3. 【断层检查】
    if (getLastLogIndex() < args.prevLogIndex) {
        std::cout << "  -> 小弟 " << me << " 拒绝日志：日志有断层！\n";
        if (needPersist) persist();
        return reply;
    }

    // 4. 【冲突检查】
    if (args.prevLogIndex < lastIncludedIndex) {
        // 大哥发来的日志太老了，我已经做成快照了，直接拒绝让他更新 nextIndex
        reply.success = false;
        return reply;
    }

    int myPrevTerm = (args.prevLogIndex == lastIncludedIndex) ? 
                    lastIncludedTerm : logs[getVectorIndex(args.prevLogIndex)].term;
                      
    if (myPrevTerm != args.prevLogTerm) {
        std::cout << "  -> 小弟 " << me << " 拒绝日志：任期冲突！\n";
        if (needPersist) persist();
        return reply;
    }

    // 5. 检查通过！开始贴日志
    int insertIndex = args.prevLogIndex + 1;
    for (int i = 0; i < args.entries.size(); i++) {
        int vecIndex = getVectorIndex(insertIndex); // 算出数组下标
        if (vecIndex < logs.size()) {
            // 如果本地已经有这个位置的日志了，看看是不是一样
            if (logs[vecIndex].term != args.entries[i].term) {
                // 发生冲突！把后面的脏数据全部一刀切掉 (截断)
                logs.erase(logs.begin() + vecIndex, logs.end());
                logs.push_back(args.entries[i]);
                needPersist = true; // 日志被修改，标记存盘
            }
        } else {
            // 本地没有，直接追加
            logs.push_back(args.entries[i]);
            needPersist = true; // 追加了新日志，标记存盘
        }
        insertIndex++;
    }

    if (needPersist) persist(); // 【重要新增】：统一执行存盘

    // 如果大哥发来了真正的日志（不是空心跳），小弟就把它存起来
    if (!args.entries.empty()) {
        std::cout << "  -> 小弟 " << me << " 成功同步了日志，最新账本写到了第 " << getLastLogIndex() << " 行\n";
    }

    if (args.leaderCommit > commitIndex) {
        // 小弟的 commitIndex 不能超过自己本地拥有的最新日志索引
        commitIndex = std::min(args.leaderCommit, getLastLogIndex());
        std::cout << "  -> 小弟 " << me << " 更新 commitIndex 为: " << commitIndex << "\n";
        
        applyLogs(); // 【重要新增】：小弟更新 commitIndex 后，立刻应用到状态机！
    }
    reply.success = true;
    // std::cout << "节点 " << me << " 收到大哥 " << args.leaderId << " 的心跳，炸弹已拆除。\n";
    return reply;
}

// 只有 Leader 才能调用这个函数
void RaftNode::updateCommitIndex() {
    // 大哥从自己最新的一条日志开始，往前倒推检查
    for (int n = getLastLogIndex(); n > std::max(commitIndex, lastIncludedIndex); n--) {
        // 只能提交自己当前任期的日志（这是 Raft 论文里的一个极端边缘情况保护）
         if (logs[getVectorIndex(n)].term != currentTerm) continue;

        int matchCount = 1; // 大哥自己肯定有这条日志，先算 1 票

        // 问问兄弟们，谁也已经同步到了第 n 行？
        for (int i = 0; i < peerCount; i++) {
            if (i == me) continue;
            if (matchIndex[i] >= n) {
                matchCount++;
            }
        }

        // 如果拥有这条日志的人数过半了！
        if (matchCount > peerCount / 2) {
            commitIndex = n; // 盖棺定论！生效！
            std::cout << "\n⭐⭐⭐ Leader " << me << " 宣布：第 " << commitIndex 
                      << " 行日志已被集群【正式提交 (Committed)】！命令是: [" 
                      << logs[commitIndex].command << "] ⭐⭐⭐\n\n";
            
            applyLogs(); // 【重要新增】：Leader 提交后，立刻应用到状态机！
            
            // 一旦找到了最高的一个可以 Commit 的位置，就不用再往前检查了
            break; 
        }
    }
}

// 【重要新增】：将已经 Commit 的日志，真正执行到数据库里
void RaftNode::applyLogs() {
    // 【修复】：防止 lastApplied 落后于快照
    if (lastApplied < lastIncludedIndex) {
        lastApplied = lastIncludedIndex;
    }

    // 只要 commitIndex 超过了已应用的索引，就不断应用
    while (lastApplied < commitIndex) {
        lastApplied++;
        LogEntry& entry = logs[getVectorIndex(lastApplied)];
        
        // 忽略占位日志或空心跳
        if (entry.command == "InitLog" || entry.command.empty()) continue;

        // 简单的命令解析：假设格式是 "SET KEY VALUE"
        std::stringstream ss(entry.command);
        std::string op, key, val;
        ss >> op >> key >> val;
        
        if (op == "SET") {
            kv_db[key] = val; // 真正写入内存数据库！
            std::cout << "[状态机执行] 节点 " << me << " 已将命令[" << entry.command 
                      << "] 写入数据库！当前 " << key << " 的值为: " << kv_db[key] << "\n";
        }
    }
}

RaftNode::~RaftNode() {
    running = false;
    if(tickerThread.joinable()) {
        tickerThread.join();
    }
}

// 将核心状态打包成 string 并存入硬盘
void RaftNode::persist() {
    std::stringstream ss;
    // 格式：Term VotedFor LogSize
    ss << currentTerm << " " << votedFor << " " 
       << lastIncludedIndex << " " << lastIncludedTerm << " " << logs.size() << "\n";
    for (const auto& entry : logs) {
        ss << entry.term << " " << entry.command << "\n";
    }
    persister->SaveRaftState(ss.str());
}

// 节点启动时，从硬盘读取并恢复状态
void RaftNode::readPersist(std::string data) {
    if (data.empty()) return;
    std::stringstream ss(data);
    int logSize;
    if (!(ss >> currentTerm >> votedFor >> lastIncludedIndex >> lastIncludedTerm >> logSize)) return;

    logs.clear();
    std::string dummy;
    std::getline(ss, dummy); // 跳过行尾换行符

    for (int i = 0; i < logSize; i++) {
        int term;
        std::string cmd;
        ss >> term;
        std::getline(ss, cmd); 
        if (!cmd.empty() && cmd[0] == ' ') cmd.erase(0, 1); // 去掉前导空格
        logs.push_back(LogEntry(term, cmd));
    }

    // 恢复 lastApplied 和 commitIndex 的底线
    lastApplied = lastIncludedIndex;
    commitIndex = lastIncludedIndex;
}

// 【修改】：初始化集群网络端口（代替以前的指针）
void RaftNode::setPeerPorts(std::vector<int>& ports) {
    peerPorts = ports;
    myPort = ports[me];
}

// 【新增】：启动 RPC 服务器，监听网络请求
void RaftNode::startRpcServer() {
    RaftRpc::startRpcServer(myPort, this, running);
}

// 暴露给外部测试用：判断自己是不是大哥
bool RaftNode::isLeader() {
    std::lock_guard<std::mutex> lock(mtx);
    return state == NodeState::LEADER;
}

// 模拟客户端向节点发送命令
void RaftNode::sendCommand(std::string cmd) {
    std::lock_guard<std::mutex> lock(mtx);
    if (state != NodeState::LEADER) {
        std::cout << "节点 " << me << " 不是 Leader，拒绝接收命令！\n";
        return;
    }

    // 1. 大哥先把命令写到自己的本地日志里
    logs.push_back(LogEntry(currentTerm, cmd));
    int newLogIndex = getLastLogIndex(); 
    
    persist(); // 【重要新增】：新命令写入日志，必须立刻持久化！
    
    std::cout << "\n[客户端请求] Leader " << me << " 接收到新命令:[" << cmd
                << "]，当前日志总数: " << newLogIndex << "\n";
    
    // 2. 接下来，大哥会在下一次发心跳时，自动把这条新日志带给小弟们
}

// 【新增】：生成快照并截断日志
void RaftNode::snapshot(int index, std::string snapshotData) {
    std::lock_guard<std::mutex> lock(mtx);
    
    // 如果请求截断的索引还没提交，或者已经被截断过了，直接忽略
    if (index <= lastIncludedIndex || index > commitIndex) {
        return;
    }

    std::cout << "\n📸 节点 " << me << " 开始生成快照！截断至真实索引: " << index << "\n";

    // 1. 算出这个真实索引在当前数组里的下标 (这就是你刚才算出来的 2)
    int vecIndex = getVectorIndex(index);
    
    // 2. 更新快照的元数据
    lastIncludedTerm = logs[vecIndex].term;
    lastIncludedIndex = index;

    // 3. 【核心】：截断数组！保留 index 之后的日志，并把 index 这条日志变成新的占位符
    std::vector<LogEntry> newLogs;
    newLogs.push_back(LogEntry(lastIncludedTerm, "InitLog")); // 新的占位符 (下标 0)
    
    for (int i = vecIndex + 1; i < logs.size(); i++) {
        newLogs.push_back(logs[i]);
    }
    logs = newLogs; // 狸猫换太子，旧数组被销毁，内存释放！

    // 4. 原子性地保存 Raft 状态和快照数据到硬盘
    std::stringstream ss;
    ss << currentTerm << " " << votedFor << " " 
       << lastIncludedIndex << " " << lastIncludedTerm << " " << logs.size() << "\n";
    for (const auto& entry : logs) {
        ss << entry.term << " " << entry.command << "\n";
    }
    
    // 调用我们在 Persister 里新写的函数
    persister->SaveStateAndSnapshot(ss.str(), snapshotData);
    
    std::cout << "📸 快照生成完毕！当前 logs 数组长度缩减为: " << logs.size() << "\n\n";
}

// 【终极 Boss 战】：小弟处理大哥砸过来的快照
InstallSnapshotReply RaftNode::handleInstallSnapshot(InstallSnapshotArgs args) {
    std::lock_guard<std::mutex> lock(mtx);
    InstallSnapshotReply reply;

    if (args.term < currentTerm) {
        reply.term = currentTerm;
        return reply;
    }

    currentTerm = args.term;
    votedFor = -1;
    state = NodeState::FOLLOWER;
    lastHeartBeat = std::chrono::system_clock::now(); // 重置炸弹！
    reply.term = currentTerm;

    // 如果我已经有这个快照（或更新的快照）了，直接忽略
    if (args.lastIncludedIndex <= lastIncludedIndex) {
        return reply;
    }

    std::cout << "\n📦 小弟 " << me << " 正在安装大哥发来的快照！截断至: " << args.lastIncludedIndex << "\n";

    // 看看我本地有没有快照末尾的那条日志
    int vecIndex = getVectorIndex(args.lastIncludedIndex);
    if (vecIndex > 0 && vecIndex < logs.size() && logs[vecIndex].term == args.lastIncludedTerm) {
        // 有！那就只截断前面的，保留后面的
        std::vector<LogEntry> newLogs;
        newLogs.push_back(LogEntry(args.lastIncludedTerm, "InitLog"));
        for (int i = vecIndex + 1; i < logs.size(); i++) {
            newLogs.push_back(logs[i]);
        }
        logs = newLogs;
    } else {
        // 没有，或者冲突了！直接把本地日志全部清空！
        logs.clear();
        logs.push_back(LogEntry(args.lastIncludedTerm, "InitLog"));
    }

    lastIncludedIndex = args.lastIncludedIndex;
    lastIncludedTerm = args.lastIncludedTerm;
    
    // 更新进度
    if (commitIndex < lastIncludedIndex) commitIndex = lastIncludedIndex;
    if (lastApplied < lastIncludedIndex) lastApplied = lastIncludedIndex;

    // 原子性保存状态和快照
    std::stringstream ss;
    ss << currentTerm << " " << votedFor << " " 
       << lastIncludedIndex << " " << lastIncludedTerm << " " << logs.size() << "\n";
    for (const auto& entry : logs) {
        ss << entry.term << " " << entry.command << "\n";
    }
    persister->SaveStateAndSnapshot(ss.str(), args.data);

    std::cout << "[状态机执行] 节点 " << me << " 已通过快照恢复数据库！\n\n";

    return reply;
}