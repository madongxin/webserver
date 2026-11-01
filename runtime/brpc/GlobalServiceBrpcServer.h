#pragma once

#include <memory>
#include <string>

namespace brpc {
class Server;
}

class GlobalServiceForwardServiceImpl;

/** GlobalService brpc：注册 GlobalServiceForward，不跑战斗 Tick / AOI */
class GlobalServiceBrpcServer {
public:
    static GlobalServiceBrpcServer &Instance();

    bool StartFromConfig();
    bool Start(const std::string &listen_addr, int idle_timeout_sec = 30);
    void Stop();
    bool running() const { return running_; }
    const std::string &listen_addr() const { return listen_addr_; }

private:
    GlobalServiceBrpcServer() = default;
    ~GlobalServiceBrpcServer();

    std::unique_ptr<brpc::Server> server_;
    std::unique_ptr<GlobalServiceForwardServiceImpl> service_;
    std::string listen_addr_;
    bool running_ = false;
};
