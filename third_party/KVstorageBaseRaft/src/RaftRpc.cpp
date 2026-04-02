#include "RaftRpc.h"
#include "RaftNode.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <thread>
#include <sstream>

// ================= 序列化与反序列化 =================

std::string RaftRpc::serializeRequestVoteArgs(RequestVoteArgs args) {
    return "RV_REQ\n" + std::to_string(args.term) + "\n" + std::to_string(args.candidateId) + "\n";
}

RequestVoteReply RaftRpc::deserializeRequestVoteReply(std::string data) {
    RequestVoteReply reply;
    std::stringstream ss(data);
    std::string type;
    int granted;
    ss >> type >> reply.term >> granted;
    reply.voteGranted = (granted == 1);
    return reply;
}

std::string RaftRpc::serializeAppendEntriesArgs(AppendEntriesArgs args) {
    std::stringstream ss;
    ss << "AE_REQ\n" << args.term << "\n" << args.leaderId << "\n"
       << args.prevLogIndex << "\n" << args.prevLogTerm << "\n"
       << args.leaderCommit << "\n" << args.entries.size() << "\n";
    for (auto& e : args.entries) {
        ss << e.term << " " << e.command << "\n";
    }
    return ss.str();
}

AppendEntriesReply RaftRpc::deserializeAppendEntriesReply(std::string data) {
    AppendEntriesReply reply;
    std::stringstream ss(data);
    std::string type;
    int successInt;
    ss >> type >> reply.term >> successInt;
    reply.success = (successInt == 1);
    return reply;
}

// ================= 底层 TCP 发送 =================

std::string RaftRpc::sendTcpRequest(int targetPort, std::string requestData) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(targetPort);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    // 100ms 超时
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000; 
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return "";
    }

    send(sock, requestData.c_str(), requestData.size(), 0);
    char buffer[4096] = {0}; // 缓冲区开大一点，防止日志太多装不下
    int bytes_read = read(sock, buffer, 4096);
    close(sock);

    if (bytes_read > 0) return std::string(buffer, bytes_read);
    return "";
}

// ================= 高级 RPC 接口 =================

RequestVoteReply RaftRpc::callRequestVote(int targetPort, RequestVoteArgs args, bool& success) {
    std::string reqData = serializeRequestVoteArgs(args);
    std::string replyData = sendTcpRequest(targetPort, reqData);
    if (replyData.empty()) { success = false; return RequestVoteReply{}; }
    success = true;
    return deserializeRequestVoteReply(replyData);
}

AppendEntriesReply RaftRpc::callAppendEntries(int targetPort, AppendEntriesArgs args, bool& success) {
    std::string reqData = serializeAppendEntriesArgs(args);
    std::string replyData = sendTcpRequest(targetPort, reqData);
    if (replyData.empty()) { success = false; return AppendEntriesReply{}; }
    success = true;
    return deserializeAppendEntriesReply(replyData);
}

// ================= RPC 服务端监听 =================

void RaftRpc::startRpcServer(int myPort, RaftNode* node, bool& running) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR , &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(myPort);

    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 10);

    std::cout << "🌐 节点 RPC 服务器已启动，正在监听端口 " << myPort << "...\n";

    while (running) {
        int new_socket = accept(server_fd, nullptr, nullptr);
        if (new_socket < 0) continue;

        std::thread([node, new_socket]() {
            char buffer[4096] = {0};
            read(new_socket, buffer, 4096);
            std::string request(buffer);
            std::string response = "";

            if (request.substr(0, 6) == "RV_REQ") {
                RequestVoteArgs args;
                std::stringstream ss(request);
                std::string type;
                ss >> type >> args.term >> args.candidateId;
                
                RequestVoteReply reply = node->handleRequestVote(args);
                response = "RV_REP\n" + std::to_string(reply.term) + "\n" + (reply.voteGranted ? "1" : "0") + "\n";
            } 
            else if (request.substr(0, 6) == "AE_REQ") {
                AppendEntriesArgs args;
                std::stringstream ss(request);
                std::string type;
                int logSize;
                ss >> type >> args.term >> args.leaderId >> args.prevLogIndex >> args.prevLogTerm >> args.leaderCommit >> logSize;
                
                std::string dummy;
                std::getline(ss, dummy); // 跳过换行符
                for (int i = 0; i < logSize; i++) {
                    int term;
                    std::string cmd;
                    ss >> term;
                    std::getline(ss, cmd);
                    if (!cmd.empty() && cmd[0] == ' ') cmd.erase(0, 1);
                    args.entries.push_back(LogEntry(term, cmd));
                }

                AppendEntriesReply reply = node->handleAppendEntries(args);
                response = "AE_REP\n" + std::to_string(reply.term) + "\n" + (reply.success ? "1" : "0") + "\n";
            } else if (request.substr(0, 6) == "IS_REQ") {
                InstallSnapshotArgs args;
                std::stringstream ss(request);
                std::string type;
                ss >> type >> args.term >> args.leaderId >> args.lastIncludedIndex >> args.lastIncludedTerm;
                
                std::string dummy;
                std::getline(ss, dummy); // 跳过换行符
                
                // 【修改】：使用 '\0' 作为分隔符，强制读取剩下的所有字符串！
                std::getline(ss, args.data, '\0'); 
                
                InstallSnapshotReply reply = node->handleInstallSnapshot(args);
                response = "IS_REP\n" + std::to_string(reply.term) + "\n";
            } else if (request.substr(0, 8) == "CLI_LOCK") {
                std::stringstream ss(request.substr(9));
                std::string res, cid;
                ss >> res >> cid;
                int code = node->attemptLock(res, cid);
                response = "LOCK_REP\n" + std::to_string(code);
            }
            else if (request.substr(0, 10) == "CLI_UNLOCK") {
                std::string res = request.substr(11);
                res.erase(res.find_last_not_of(" \n\r\t") + 1);
                node->releaseLock(res);
                response = "LOCK_REP\nOK";
            }

            send(new_socket, response.c_str(), response.size(), 0);
            close(new_socket);
        }).detach();
    }
}

std::string RaftRpc::serializeInstallSnapshotArgs(InstallSnapshotArgs args) {
    return "IS_REQ\n" + std::to_string(args.term) + "\n" + std::to_string(args.leaderId) + "\n"
           + std::to_string(args.lastIncludedIndex) + "\n" + std::to_string(args.lastIncludedTerm) + "\n"
           + args.data + "\n";
}

InstallSnapshotReply RaftRpc::deserializeInstallSnapshotReply(std::string data) {
    InstallSnapshotReply reply;
    std::stringstream ss(data);
    std::string type;
    ss >> type >> reply.term;
    return reply;
}

InstallSnapshotReply RaftRpc::callInstallSnapshot(int targetPort, InstallSnapshotArgs args, bool& success) {
    std::string reqData = serializeInstallSnapshotArgs(args);
    std::string replyData = sendTcpRequest(targetPort, reqData);
    if (replyData.empty()) { success = false; return InstallSnapshotReply{}; }
    success = true;
    return deserializeInstallSnapshotReply(replyData);
}