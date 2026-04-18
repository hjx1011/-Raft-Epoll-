// GatewayMain.cpp
#include "EpollServer.h"
#include "CacheManager.h"
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <signal.h> // 【新增】
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/basic_file_sink.h"

HighAvailableCacheManager g_manager;

void SendHttpResponse(int client_fd, int status_code, const std::string& body) {
    // 1. 匹配标准的 HTTP 状态描述
    std::string status_text = "OK";
    if (status_code == 400) status_text = "Bad Request";
    else if (status_code == 404) status_text = "Not Found";
    else if (status_code == 500) status_text = "Internal Server Error";

    // 2. 拼接极其标准的 HTTP 报文
    std::string response = "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n";
    response += "Content-Type: text/plain; charset=utf-8\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: keep-alive\r\n\r\n";
    response += body;
    
    // 3. 安全的非阻塞写入
    size_t total_written = 0;
    while (total_written < response.size()) {
        ssize_t n = write(client_fd, response.c_str() + total_written, response.size() - total_written);
        if (n > 0) {
            total_written += n;
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; // 缓冲区满，跳出等待下次 epoll 触发
        } else {
            break; // 发生错误或连接断开
        }
    }
}

void EpollServer::ProcessHttpRequest(int client_fd, const std::string& request) {
    std::istringstream iss(request);
    std::string method, path, version;
    iss >> method >> path >> version;

    spdlog::debug("\n[Gateway 收到请求] {} , {}", method, path);

    if (method == "GET" && path.find("/get?key=") == 0) {
        std::string key = path.substr(9);
        std::string val = g_manager.get_data(key);
        
        if (!val.empty()) {
            SendHttpResponse(client_fd, 200, val + "\n");
        } else {
            SendHttpResponse(client_fd, 404, "Key Not Found\n");
        }
    } 
    else if (method == "POST" && path.find("/set?") == 0) {
        size_t key_pos = path.find("key=") + 4;
        size_t and_pos = path.find("&val=");
        if (key_pos != std::string::npos && and_pos != std::string::npos) {
            std::string key = path.substr(key_pos, and_pos - key_pos);
            std::string val = path.substr(and_pos + 5);

            if (g_manager.write_data(key, val)) {
                SendHttpResponse(client_fd, 200, "Set Success\n");
            } else {
                SendHttpResponse(client_fd, 500, "Raft Cluster Error / No Leader\n");
            }
        } else {
            SendHttpResponse(client_fd, 400, "Bad Request Format\n");
        }
    } else {
        SendHttpResponse(client_fd, 400, "Bad Request\n");
    }
}

int main() {
    // 【修复】忽略 SIGPIPE 信号，防止客户端强行断开连接时导致网关进程崩溃
    signal(SIGPIPE, SIG_IGN);

    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t][%^%l%$] %v");

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/gateway.log", true);

    auto logger = std::make_shared<spdlog::logger>("multi_sink",spdlog::sinks_init_list{console_sink,file_sink});

    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::err);
    spdlog::flush_on(spdlog::level::err); 

    spdlog::info("==================================================");
    spdlog::info("🚀 终极形态：分布式 KV 存储 API 网关已启动");
    spdlog::info("📡 基于 Epoll + Reactor 异步网络模型");
    spdlog::info("==================================================");
    
    try{
        EpollServer server(8081);
        server.start();
    } catch (const std::exception& e) {
        spdlog::critical("网关发生致命错误并崩溃: {}",e.what());
    }
    
    return 0;
}