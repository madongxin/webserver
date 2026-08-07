#pragma once

#include <memory>
#include <string>

namespace brpc {
class Server;
}

class WorldForwardServiceImpl;

/** World 中控 brpc：注册 WorldForward，不跑战斗 Tick / AOI */
class WorldBrpcServer {
public:
    static WorldBrpcServer &Instance();

    bool StartFromConfig();
    bool Start(const std::string &listen_addr, int idle_timeout_sec = 30);
    void Stop();
    bool running() const { return running_; }
    const std::string &listen_addr() const { return listen_addr_; }

private:
    WorldBrpcServer() = default;
    ~WorldBrpcServer();

    std::unique_ptr<brpc::Server> server_;
    std::unique_ptr<WorldForwardServiceImpl> service_;
    std::string listen_addr_;
    bool running_ = false;
};
