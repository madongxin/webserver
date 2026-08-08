#pragma once

/**
 * @file TcpConnection.h
 * @brief 单条 TCP 连接：Channel + 读写 Buffer + 业务回调
 *
 * 生命周期：
 *   构造（注册 ET 读/写回调，尚未加入 epoll）
 *   -> ConnectionEstablished（Tie + EnableRead + on_connect_）
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

    std::function<void(const std::shared_ptr<TcpConnection> &)> on_close_;
    std::function<void(const std::shared_ptr<TcpConnection> &)> on_message_;
    std::function<void(const std::shared_ptr<TcpConnection> &)> on_connect_;

    void ReadNonBlocking();
    void WriteNonBlocking();
    void CloseForBackpressure(const char *reason);

    std::unique_ptr<HttpContext> context_;
    TimeStamp timestamp_;
};
