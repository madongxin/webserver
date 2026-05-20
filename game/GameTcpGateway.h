#pragma once

#include <memory>
#include <string>
#include <thread>

class TcpConnection;

class GameTcpGateway {
public:
    GameTcpGateway(const std::string &ip, int port);
    ~GameTcpGateway();

    void StartInBackground();

private:
    void Run();
    void OnMessage(const std::shared_ptr<TcpConnection> &conn);

    std::string ip_;
    int port_ = 0;
    std::thread thread_;
};
