#pragma once
#include <sys/epoll.h>
#include <string>
#include "ThreadPool.h"

class EpollServer {
private:
    int listen_fd_;
    int epoll_fd_;
    ThreadPool thread_pool_;

    void SetNonBlocking(int fd);
    void AcceptConnection();
    void HandleRead(int client_fd);

    void ProcessHttpRequest(int client_fd, const std::string& request);

public:
    EpollServer(int port);
    ~EpollServer();
    void start();
};