#pragma once

#include "ReplySink.h"

#include <memory>
#include <string>

class EventLoop;
class TcpConnection;

/**
 * 从发送缓冲里丢掉只含移动的 AOI 帧，给全量快照和 ENTER/LEAVE 腾位置。
 * keep_newest 为真时留下最后一帧移动，避免观察者停在进图那一帧。
 */
size_t DropQueuedAoiMoveFrames(TcpConnection *conn, bool keep_newest = false);
/**
 * 心跳和 RPC 回包写出前调用。应答插到队首；多出来的旧移动帧丢掉，
 * 但至少留一帧最新位置。不因为发送缓冲积压关连接。
 */
void MakeRoomForControl(TcpConnection *conn);

/**
 * 只在连接自己的 EventLoop 上调用。
 * 1 = 整帧已经离开应用缓冲。2 = 还在应用缓冲里，下一条要等它离开。
 * 0 = 还连着，回包在有序队列里等补发。小于 0 = 连接已关。
 * kind 必须是字符串字面量。
 */
int SubmitControlOnLoop(const std::shared_ptr<TcpConnection> &conn, uint64_t seq, const char *kind,
                        const std::string &frame);

/** 将 Send 回投到连接所属 EventLoop，保证只在 IO 线程改 TcpConnection */
class TcpReplySink : public ReplySink {
public:
    explicit TcpReplySink(const std::shared_ptr<TcpConnection> &conn);

    void SendFrame(const std::string &response_frame) override;
    void CloseConnection() override;
    std::shared_ptr<TcpConnection> tcp_connection() const override;

private:
    std::weak_ptr<TcpConnection> conn_;
    EventLoop *loop_ = nullptr;
};
