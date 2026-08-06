#pragma once

/**
 * @file BrpcGameServer.h
 * @brief 进程内 brpc Server：注册 MailBrpcService，与 GameTcpGateway 双栈并存
 */

#include <memory>
#include <string>

namespace brpc {
class Server;
}

class MailBrpcServiceImpl;

class BrpcGameServer {
public:
    static BrpcGameServer &Instance();

    /** 读 config/brpc.cnf 并 Start；失败返回 false */
    bool StartFromConfig();

    /** 指定地址启动，如 0.0.0.0:8181 */
    bool Start(const std::string &listen_addr, int idle_timeout_sec = 30);

    void Stop();
    bool running() const { return running_; }
    const std::string &listen_addr() const { return listen_addr_; }

private:
    BrpcGameServer() = default;
    ~BrpcGameServer();

    std::unique_ptr<brpc::Server> server_;
    std::unique_ptr<MailBrpcServiceImpl> service_;
    std::string listen_addr_;
    bool running_ = false;
};
