#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <atomic>
#include <string>
#include <random> // 【新增】引入随机数库

std::atomic<int> success_count(0);
std::atomic<int> fail_count(0);

// 【核心修改】定义测试的商品总数（键池大小）
// 10000 个不同的商品，足以触发 ARC 缓存的淘汰机制，并打散分段锁
const int KEY_POOL_SIZE = 10000; 

void worker(int thread_id, int requests_per_thread, bool is_write) {
    int sock = -1;
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8081); // 确认是否是 8081 端口
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    // 为每个线程初始化一个独立的随机数生成器，防止多线程竞争
    std::mt19937 rng(std::random_device{}() + thread_id);
    std::uniform_int_distribution<int> dist(1, KEY_POOL_SIZE);

    auto connect_server = [&]() {
        if (sock != -1) close(sock);
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;
        struct timeval tv;
        tv.tv_sec = 2; tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

        if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            close(sock); sock = -1; return false;
        }
        return true;
    };

    connect_server();

    for (int i = 0; i < requests_per_thread; ++i) {
        if (sock == -1 && !connect_server()) {
            fail_count++; continue;
        }

        std::string req;
        // 【核心修改】每次请求随机生成一个 1 ~ 10000 的商品 ID
        int random_item_id = dist(rng);
        std::string target_key = "item_" + std::to_string(random_item_id); 
        
        if (is_write) {
            // 写压测：给随机选中的商品更新库存数据
            std::string val = "stock_" + std::to_string(random_item_id) + "_update_" + std::to_string(i);
            req = "POST /set?key=" + target_key + "&val=" + val + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n";
        } else {
            // 读压测：随机读取一个商品信息
            req = "GET /get?key=" + target_key + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n";
        }

        if (send(sock, req.c_str(), req.length(), 0) <= 0) {
            fail_count++; connect_server(); continue;
        }

        char buffer[1024] = {0};
        int bytes_read = read(sock, buffer, sizeof(buffer) - 1);

        if (bytes_read > 0) {
            if (strncmp(buffer, "HTTP/1.1 200", 12) == 0) {
                success_count++;
            } else {
                fail_count++;
            }
        } else {
            fail_count++; connect_server();
        }
    }
    if (sock != -1) close(sock);
}

int main(int argc, char* argv[]) {
    int num_threads = 50;
    int requests_per_thread = 200;
    bool is_write = false;

    if (argc > 1) num_threads = std::stoi(argv[1]);
    if (argc > 2) requests_per_thread = std::stoi(argv[2]);
    if (argc > 3) is_write = (std::string(argv[3]) == "write");

    std::cout << "========================================\n";
    std::cout << "🚀 高并发秒杀系统压测工具 (真实离散数据版)...\n";
    std::cout << "🎯 压测模式: " << (is_write ? "写 (POST /set)" : "读 (GET /get)") << "\n";
    std::cout << "🧵 并发线程数: " << num_threads << "\n";
    std::cout << "🔄 每个线程请求数: " << requests_per_thread << "\n";
    std::cout << "🎲 随机键池大小: " << KEY_POOL_SIZE << " 个商品\n";
    std::cout << "----------------------------------------\n";

    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i, requests_per_thread, is_write);
    }

    for (auto& t : threads) { t.join(); }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    int total_requests = success_count + fail_count;
    double qps = total_requests / elapsed.count();

    std::cout << "\n📊 压测结果报告:\n";
    std::cout << "⏱️  总耗时: " << elapsed.count() << " 秒\n";
    std::cout << "✅  成功请求: " << success_count << "\n";
    std::cout << "❌  失败请求: " << fail_count << "\n";
    std::cout << "⚡  QPS: " << qps << " req/sec\n";
    std::cout << "========================================\n";

    return 0;
}