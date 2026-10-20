#pragma once

/**
 * @file TcpConnection.h
 * @brief 单条 TCP 连接：Channel + 读写 Buffer + 业务回调
 *
 * 生命周期：
 *   构造（创建 Channel、注册读/写回调，尚未加入 epoll）
 *   -> 所属 EventLoop 上 ConnectionEstablished（Tie + on_connect_；仍 Connected 才 EnableET/Read）
 *   -> HandleMessage 循环读直到 EAGAIN，触发 on_message_
 *   -> Send 可能触发 EPOLLOUT
 *   -> HandleClose -> on_close_ -> 主线程删 map -> ConnectionDestructor
 */

#include "common.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "TimeStamp.h"

class Buffer;
class HttpContext;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    enum ConnectionState {
        Invalid = 1,
        Connected,
        Disconected
    };

    DISALLOW_COPY_AND_MOVE(TcpConnection);

    TcpConnection(EventLoop *loop, int connfd, uint64_t connid);
    ~TcpConnection();

    void ConnectionEstablished();
    void ConnectionDestructor();

    void set_connection_callback(std::function<void(const std::shared_ptr<TcpConnection> &)> const &fn);
    void set_close_callback(std::function<void(const std::shared_ptr<TcpConnection> &)> const &fn);
    void set_message_callback(std::function<void(const std::shared_ptr<TcpConnection> &)> const &fn);

    Buffer *read_buf();
    Buffer *send_buf();
    /** 应用层帧流缓冲（如 ProtoFraming），随连接生命周期清理 */
    std::string &proto_stream();

    void set_max_read_buf_bytes(size_t n);
    void set_max_send_buf_bytes(size_t n);
    size_t max_read_buf_bytes() const;
    size_t max_send_buf_bytes() const;
    bool force_closed_for_backpressure() const;

    void Read();
    void Write();

    void Send(const std::string &msg);
    void Send(const char *msg, int len);
    void Send(const char *msg);
    /**
     * 尽力发送；发送队列将超限时返回 false 且不关闭连接。
     * 用于 AOI/分线等可丢弃推送，避免背压杀连接导致 MarkDisconnected 雪崩。
     */
    bool TrySend(const std::string &msg);
    bool TrySend(const char *msg, int len);
    /** 可靠推送（ENTER/LEAVE）：软丢水位更高（约 90%），尽量送达，仍不关连接。 */
    bool TrySendUrgent(const std::string &msg);
    bool TrySendUrgent(const char *msg, int len);
    /** 心跳应答：用满发送缓冲仍不关连接。缓冲已满才返回 false。 */
    bool TrySendControl(const std::string &msg);
    bool TrySendControl(const char *msg, int len);
    void SendFile(int filefd, int size);

    /** epoll 读就绪：读 socket -> 调 on_message_ */
    void HandleMessage();
    /** epoll 写就绪：把 send_buf_ 写入 socket */
    void HandleWrite();
    void HandleClose();

    ConnectionState state() const;
    EventLoop *loop() const;
    int fd() const;
    uint64_t id() const;
    HttpContext *context() const;
    bool IsWriting() const;

    TimeStamp timestamp() const;
    void UpdateTimeStamp(TimeStamp now);

private:
    int connfd_;
    uint64_t connid_;
    ConnectionState state_;
    EventLoop *loop_;

    std::unique_ptr<Channel> channel_;
    std::unique_ptr<Buffer> read_buf_;
    std::unique_ptr<Buffer> send_buf_;
    std::string proto_stream_;

    size_t max_read_buf_bytes_;
    size_t max_send_buf_bytes_;
    bool force_closed_for_backpressure_;
    bool in_close_;

    std::function<void(const std::shared_ptr<TcpConnection> &)> on_close_;
    std::function<void(const std::shared_ptr<TcpConnection> &)> on_message_;
    std::function<void(const std::shared_ptr<TcpConnection> &)> on_connect_;

    void ReadNonBlocking();
    void WriteNonBlocking();
    /** 关闭路径专用：只写 socket，失败不回调 HandleClose。 */
    void FlushSendBufBestEffort();
    void CloseForBackpressure(const char *reason);
    /** soft_drop=true：队列将满时丢弃本帧并返回 false；false：关连接（背压）。
     *  urgent：软丢水位约 90%（ENTER/LEAVE）。control：心跳，水位 100%。 */
    bool SendInternal(const char *msg, int len, bool soft_drop, bool urgent = false,
                      bool control = false);

    std::unique_ptr<HttpContext> context_;
    TimeStamp timestamp_;
};
