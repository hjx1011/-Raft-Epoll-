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

EpollServer::EpollServer(int port) : thread_pool_(16) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    if (bind(listen_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        spdlog::critical("🔴 致命错误：端口 {} 已被占用！请先杀掉旧的网关进程！", port);
        exit(EXIT_FAILURE); // 端口被占用直接退出，绝不静默失败
    }

    listen(listen_fd_, SOMAXCONN);
    
    // 【修改点 1】listen_fd_ 也必须设为非阻塞，因为 ET 模式必须配合非阻塞
    SetNonBlocking(listen_fd_);

    epoll_fd_ = epoll_create1(0);

    struct epoll_event event;
    event.data.fd = listen_fd_;
    // 【修改点 2】监听套接字也使用边缘触发 (EPOLLET)
    event.events = EPOLLIN | EPOLLET; 
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event);

    spdlog::info("Epoll Server (ET Mode) started on port: {}", port);
}

EpollServer::~EpollServer() {
    close(listen_fd_);
    close(epoll_fd_);
}

void EpollServer::SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void EpollServer::start() {
    struct epoll_event events[MAX_EVENTS];

    while (true) {
        int num_events = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);

        for (int i = 0; i < num_events; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd_) {
                // 有新连接到来
                AcceptConnection();
            } else if (events[i].events & EPOLLIN) {
                thread_pool_.enqueue([this, fd]() {
                    this->HandleRead(fd);
                });
            }
        }
    }
}

// 【关键修改点 3】由于 listen_fd 是 ET 模式，必须用 while 循环接受所有连接
void EpollServer::AcceptConnection() {
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd > 0) {
            SetNonBlocking(client_fd);
            struct epoll_event event;
            event.data.fd = client_fd;
            // 使用 ET + ONESHOT
            event.events = EPOLLIN | EPOLLET | EPOLLONESHOT; 
            epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event);
            // spdlog::debug("Accepted new connection: {}", client_fd);
        } else {
            // 如果返回 EAGAIN，说明连接池里的新连接已经接完了
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; 
            } else {
                spdlog::error("Accept error!");
                break;
            }
        }
    }
}

void EpollServer::HandleRead(int client_fd) {
    char buffer[BUFFER_SIZE];
    std::string request_data;
    bool client_closed = false; 

    while (true) {
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            request_data += buffer;
        } else if (bytes_read == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 数据读完了
            break;
        } else {
            client_closed = true;
            break;
        }
    }

    if (!request_data.empty()) {
        ProcessHttpRequest(client_fd, request_data);
    }

    if (client_closed) {
        close(client_fd);
    } else {
        struct epoll_event event;
        event.data.fd = client_fd;
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, client_fd, &event);
    }
}