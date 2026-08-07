#pragma once

#include <memory>
#include <string>

namespace brpc {
class Server;
}

class GameLogicForwardServiceImpl;
class GameLogicServiceImpl;

class GameLogicBrpcServer {
public:
    static GameLogicBrpcServer &Instance();

    bool StartFromConfig();
    bool Start(const std::string &listen_addr, int idle_timeout_sec = 30);
    void Stop();

    bool running() const { return running_; }
    const std::string &listen_addr() const { return listen_addr_; }

private:
    GameLogicBrpcServer() = default;
    ~GameLogicBrpcServer();

    std::unique_ptr<brpc::Server> server_;
    std::unique_ptr<GameLogicForwardServiceImpl> service_;
    std::unique_ptr<GameLogicServiceImpl> gl_service_;
    std::string listen_addr_;
    bool running_ = false;
};
