#include "EpollServer.h"
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstring>
#include "spdlog/spdlog.h"

#define MAX_EVENTS 1024
#define BUFFER_SIZE 4096

EpollServer::EpollServer(int port) : thread_pool_(4) { // 4个工作线程
    // 1. 创建监听 Socket
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    
    // 设置端口复用
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. 绑定 IP 和 端口
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    bind(listen_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));

    // 3. 开始监听
    listen(listen_fd_, SOMAXCONN);

    // 4. 创建 epoll 实例
    epoll_fd_ = epoll_create1(0);

    // 5. 将 listen_fd 加入 epoll，监听读事件 (EPOLLIN)
    struct epoll_event event;
    event.data.fd = listen_fd_;
    event.events = EPOLLIN; // listen_fd 一般用水平触发(LT)即可
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event);

    spdlog::info("Epoll Server started on port: {}", port);
}

EpollServer::~EpollServer() {
    close(listen_fd_);
    close(epoll_fd_);
}

// 设置文件描述符为非阻塞 (Non-blocking)
void EpollServer::SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void EpollServer::start() {
    struct epoll_event events[MAX_EVENTS];

    while (true) {
        // 阻塞等待事件发生
        int num_events = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);

        for (int i = 0; i < num_events; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd_) {
                // 有新连接到来
                AcceptConnection();
            } else if (events[i].events & EPOLLIN) {
                // 客户端发来数据，交给线程池处理，避免阻塞 epoll 主循环
                thread_pool_.enqueue([this, fd]() {
                    this->HandleRead(fd);
                });
            }
        }
    }
}

void EpollServer::AcceptConnection() {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, &client_len);
    
    if (client_fd > 0) {
        SetNonBlocking(client_fd); // 必须设置为非阻塞
        
        // 将新连接加入 epoll，使用边缘触发 (EPOLLET)
        struct epoll_event event;
        event.data.fd = client_fd;
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT; // EPOLLONESHOT 保证一个 socket 同一时刻只被一个线程处理
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event);
    }
}

void EpollServer::HandleRead(int client_fd) {
    char buffer[BUFFER_SIZE];
    std::string request_data;

    // 因为是 ET 模式 + 非阻塞，必须一次性把数据读完，直到返回 EAGAIN
    while (true) {
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            request_data += buffer;
        } else if (bytes_read == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 数据读完了
            break;
        } else {
            // 客户端断开连接或出错
            close(client_fd);
            return;
        }
    }

    if (!request_data.empty()) {
        // 调用业务逻辑处理 HTTP 请求
        ProcessHttpRequest(client_fd, request_data);
    }

    // 处理完后，重新重置 EPOLLONESHOT，以便下次继续监听该 socket
    struct epoll_event event;
    event.data.fd = client_fd;
    event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, client_fd, &event);
}