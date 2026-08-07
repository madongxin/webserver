#pragma once

#include <memory>
#include <string>

namespace brpc {
class Server;
}

class SessionServiceImpl;
class AuthServiceImpl;

/** Session 进程同时承载 AuthService + SessionService（逻辑分离、同二进制）。 */
class SessionBrpcServer {
public:
    static SessionBrpcServer &Instance();
    bool StartFromConfig();
    bool Start(const std::string &listen_addr, int idle_timeout_sec = 30);
    void Stop();
    bool running() const { return running_; }
    const std::string &listen_addr() const { return listen_addr_; }

private:
    SessionBrpcServer() = default;
    ~SessionBrpcServer();
    std::unique_ptr<brpc::Server> server_;
    std::unique_ptr<SessionServiceImpl> service_;
    std::unique_ptr<AuthServiceImpl> auth_service_;
    std::string listen_addr_;
    bool running_ = false;
};
