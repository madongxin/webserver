/**
 * @file Acceptor.cpp
 * @brief 监听套接字实现：非阻塞 listenfd + accept4 非阻塞客户端 fd
 */

#include "Acceptor.h"
#include "Channel.h"
#include "EventLoop.h"
#include "Logging.h"
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cassert>
#include <cerrno>
#include <cstring>

Acceptor::Acceptor(EventLoop *loop, const char *ip, const int port)
    : loop_(loop), listenfd_(-1) {
    Create();
    Bind(ip, port);
    Listen();
    channel_ = std::make_unique<Channel>(listenfd_, loop);
    channel_->set_read_callback(std::bind(&Acceptor::AcceptConnection, this));
    channel_->EnableRead();  // 注册到主 reactor 的 epoll
}

Acceptor::~Acceptor() {
    loop_->DeleteChannel(channel_.get());
    ::close(listenfd_);
}

void Acceptor::Create() {
    assert(listenfd_ == -1);
    listenfd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (listenfd_ == -1)
        LOG_ERROR << "Failed to create socket";
    int on = 1;
    ::setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
}

void Acceptor::Bind(const char *ip, const int port) {
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip);
    addr.sin_port = htons(port);
    if (::bind(listenfd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == -1) {
        LOG_FATAL << "Failed to Bind [" << ip << ":" << port << "] errno=" << errno << " "
                  << strerror(errno)
                  << " (port in use? stop old server: pkill -f build/test/server)";
    }
}

void Acceptor::Listen() {
    if (listenfd_ < 0)
        return;
    if (::listen(listenfd_, SOMAXCONN) == -1)
        LOG_FATAL << "Failed to Listen errno=" << errno << " " << strerror(errno);
}

void Acceptor::AcceptConnection() {
    struct sockaddr_in client {};
    socklen_t client_addrlength = sizeof(client);
    assert(listenfd_ != -1);

    // ET 模式下 listen 可读时应循环 accept 直到 EAGAIN；此处单次 accept4
    int clnt_fd =
        ::accept4(listenfd_, reinterpret_cast<struct sockaddr *>(&client), &client_addrlength,
                  SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (clnt_fd == -1) {
        LOG_ERROR << "Failed to Accept";
        return;
    }
    if (new_connection_callback_)
        new_connection_callback_(clnt_fd);  // -> TcpServer::HandleNewConnection
}

void Acceptor::set_newconnection_callback(std::function<void(int)> const &callback) {
    new_connection_callback_ = std::move(callback);
}
