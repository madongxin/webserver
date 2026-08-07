#pragma once

#include "ReplySink.h"

#include <memory>
#include <string>

class EventLoop;
class TcpConnection;

/** 将 Send 回投到连接所属 EventLoop，保证只在 IO 线程改 TcpConnection */
class TcpReplySink : public ReplySink {
public:
    explicit TcpReplySink(const std::shared_ptr<TcpConnection> &conn);

    void SendFrame(const std::string &response_frame) override;

private:
    std::weak_ptr<TcpConnection> conn_;
    EventLoop *loop_ = nullptr;
};
