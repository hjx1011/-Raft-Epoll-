// GatewayMain.cpp
#include "EpollServer.h"
#include "CacheManager.h"
#include <iostream>
#include <sstream>
#include <unistd.h>
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/basic_file_sink.h"
// 全局实例化高可用缓存管理器
HighAvailableCacheManager g_manager;

// 简单的 HTTP 响应封装
void SendHttpResponse(int client_fd, int status_code, const std::string& body) {
    std::string response = "HTTP/1.1 " + std::to_string(status_code) + " OK\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: keep-alive\r\n\r\n";
    response += body;
    write(client_fd, response.c_str(), response.size());
}

// 实现 EpollServer 中声明的业务处理函数
void EpollServer::ProcessHttpRequest(int client_fd, const std::string& request) {
    std::istringstream iss(request);
    std::string method, path, version;
    iss >> method >> path >> version;

    spdlog::info("\n[Gateway 收到请求] {} , {}", method, path);

    if (method == "GET" && path.find("/get?key=") == 0) {
        std::string key = path.substr(9);
        
        // 调用 CacheManager 获取数据 (内部会自动处理缓存和 Raft)
        std::string val = g_manager.get_data(key);
        
        if (!val.empty()) {
            SendHttpResponse(client_fd, 200, val);
        } else {
            SendHttpResponse(client_fd, 404, "Key Not Found");
        }
    } 
    else if (method == "POST" && path.find("/set?") == 0) {
        size_t key_pos = path.find("key=") + 4;
        size_t and_pos = path.find("&val=");
        if (key_pos != std::string::npos && and_pos != std::string::npos) {
            std::string key = path.substr(key_pos, and_pos - key_pos);
            std::string val = path.substr(and_pos + 5);

            // 调用 CacheManager 写入数据
            if (g_manager.write_data(key, val)) {
                SendHttpResponse(client_fd, 200, "Set Success");
            } else {
                SendHttpResponse(client_fd, 500, "Raft Cluster Error / No Leader");
            }
        } else {
            SendHttpResponse(client_fd, 400, "Bad Request Format");
        }
    } else {
        SendHttpResponse(client_fd, 400, "Bad Request");
    }
}

int main() {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t][%^%1%$] %v");

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/gateway.log", true);

    // 2. 创建一个同时输出到屏幕和文件的 Logger
    // spdlog::logger logger("multi_sink", {console_sink,file_sink});
    // spdlog::set_default_logger(std::make_shared<spdlog::logger>(logger));

    auto logger = std::make_shared<spdlog::logger>("multi_sink",spdlog::sinks_init_list{console_sink,file_sink});

    spdlog::set_default_logger(logger);
    // 3. 设置全局日志级别（低于这个级别的日志将不会被处理，极大提升性能）
    spdlog::set_level(spdlog::level::info);// 上线时可以改成 level::warn

    spdlog::flush_on(spdlog::level::info); 

    spdlog::info("==================================================");
    spdlog::info("🚀 终极形态：分布式 KV 存储 API 网关已启动");
    spdlog::info("📡 基于 Epoll + Reactor 异步网络模型");
    spdlog::info("==================================================");
    
    try{
        // 启动网关服务器，监听 8080 端口
        EpollServer server(8080);
        server.start();
    } catch (const std::exception& e) {
        spdlog::critical("网关发生致命错误并崩溃: {}",e.what());
    }
    
    return 0;
}