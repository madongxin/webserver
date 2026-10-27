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
    /** 写出过字节且应用缓冲降到可靠水位以下时调用。只在所属 EventLoop 上触发。 */
    void set_drain_callback(std::function<void()> fn);

    Buffer *read_buf();
    Buffer *send_buf();
    /** 拷贝尚未写出的字节。只在所属 EventLoop 上调用。 */
    std::string CopySendBuf();
    /** 用 bytes 替换尚未写出的字节。只在所属 EventLoop 上调用。 */
    void ReplaceSendBuf(const std::string &bytes);
    /**
     * 已经写进内核、但应用缓冲里还剩尾巴的那一帧必须先写完。
     * 这个长度之后的字节才是完整帧，可以丢掉或插到心跳前面。
     * 只在所属 EventLoop 上调用。
     */
    size_t send_partial_prefix() const;
    /** 保留未写完的帧头，用 queued 换掉后面的完整帧。只在所属 EventLoop 上调用。 */
    void ReplaceQueuedAfterPrefix(const std::string &queued);
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
    /**
     * 心跳和 RPC 回包。
     * Closed：连接没了。Blocked：这次没写进缓冲，连接还在。
     * WrittenDirect：整帧已经离开应用缓冲。
     * Buffered：整帧或部分字节还在应用缓冲里，但已经按序进入发送流。
     * 下一条回包直接接在已有控制帧后面，不等这一帧离开缓冲。
     */
    enum ControlSendResult { Closed = -1, Blocked = 0, WrittenDirect = 1, Buffered = 2 };
    ControlSendResult TrySendControl(const std::string &msg, bool over_cap = false);
    ControlSendResult TrySendControl(const char *msg, int len, bool over_cap = false);
    /** 应用缓冲里还有还没写进内核的控制字节。不阻止下一条控制回包进入发送流。 */
    bool ControlReplyBuffered() const { return control_hold_end_ > 0; }
    size_t control_run() const;
    size_t control_hold_end() const;
    /**
     * 每个连接最多挂一帧最新移动。调用方先丢掉缓冲里的旧移动帧。
     * 内核里还没送出的字节超过 1KB 时，这一帧先留在应用缓冲末尾，不立刻 write，
     * 避免回包排在旧坐标后面。200ms 后，连接没有重传时定时器把它交给写路径。
     * 没写完的字节留在缓冲里，等 EPOLLOUT，这期间不能删掉；前面的回包超过 4KB 就先排队。
     * 非移动数据已经超过 64KB 时返回 false，调用方再试。
     * 不关连接。
     */
    bool TrySendLatestMove(const std::string &msg);
    /** 最新移动帧已经交给写路径，但还没全部写进内核。这时不能从缓冲里删掉它。 */
    bool latest_move_awaiting_write() const { return send_move_sticky_; }
    void SendFile(int filefd, int size);

    /** epoll 读就绪：读 socket -> 调 on_message_ */
    void HandleMessage();
    /** epoll 写就绪：把 send_buf_ 写入 socket */
    void HandleWrite();
    void HandleClose();

    ConnectionState state() const;
    EventLoop *loop() const;
    int fd() const;
    /** 内核里还没交给网卡的字节。只在所属 EventLoop 上调用。 */
    int unsent_bytes() const;
    /** 最新移动帧正压在应用缓冲末尾，还没 write。只在所属 EventLoop 上调用。 */
    bool holding_latest_move() const { return send_hold_move_; }
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
    /** send_buf_ 开头有多少字节属于已经部分写进内核的那一帧。 */
    size_t send_partial_prefix_ = 0;
    /** 半帧前缀本身是控制回包的剩余字节。这段不能再算进 control_run_。 */
    bool partial_is_control_ = false;
    /**
     * 紧接在半帧前缀后面的完整 RPC 回包字节。新的回包接在这段后面，
     * 不能插到更早的回包前面，也不能排到这段后面的 MOVE 后面。
     * 半帧前缀里的字节不在这里重复计算。
     */
    size_t control_run_ = 0;
    /**
     * 从缓冲开头数，控制字节的结束位置。等于半帧前缀（若它是控制帧）加上 control_run_，
     * 两段不重叠。控制帧写出后立刻变成 0，不依赖后面的 MOVE 写出。
     */
    size_t control_hold_end_ = 0;
    /**
     * 应用缓冲末尾有一帧最新移动。内核未发送字节还多时先不写它。
     * send_move_hold_off_ 是这帧在缓冲里的起点；前面的字节仍要写出（半帧或回包）。
     */
    bool send_hold_move_ = false;
    size_t send_move_hold_off_ = 0;
    /** 定时器已经决定写出这帧。写到 EAGAIN 时留在缓冲里，等 EPOLLOUT，禁止再删。 */
    bool send_move_sticky_ = false;
    /** 这一帧开始停放的时刻。超过 1 秒仍停着，就写出这一帧，避免位置一直不到客户端。 */
    int64_t send_hold_since_ms_ = 0;
    /** 每个连接最多一个 200ms 定时器，用来在关掉 EPOLLOUT 之后再试暂存的移动帧。 */
    bool move_retry_armed_ = false;

    std::function<void(const std::shared_ptr<TcpConnection> &)> on_close_;
    std::function<void(const std::shared_ptr<TcpConnection> &)> on_message_;
    std::function<void(const std::shared_ptr<TcpConnection> &)> on_connect_;
    std::function<void()> on_drain_;

    void ReadNonBlocking();
    void WriteNonBlocking();
    /** 还有必须写出的字节才注册 EPOLLOUT。只剩压住的移动帧则关掉，避免水平触发空转。 */
    void ArmWriteIfNeeded();
    void ArmParkedMoveRetry();
    void OnParkedMoveTimer();
    /** 关闭路径专用：只写 socket，失败不回调 HandleClose。 */
    void FlushSendBufBestEffort();
    void CloseForBackpressure(const char *reason);
    void ConsumeSendPrefix(int n);
    /** 用互不重叠的半帧前缀和 control_run_ 重算控制字节结束位置。 */
    void RefreshControlHoldEnd();
    /** write 跨过帧边界时，把缓冲开头仍未写完的那一帧记成半帧前缀。 */
    void NoteWriteConsumed(const char *data, size_t data_len, size_t written);
    /** 内核里还没交给网卡的字节。拿不到时返回 0，不用未确认字节充数。 */
    int UnsentBytes() const;
    /** soft_drop=true：队列将满时丢弃本帧并返回 false；false：关连接（背压）。
     *  urgent：软丢水位约 90%（ENTER/LEAVE）。control：心跳，水位 100%。 */
    bool SendInternal(const char *msg, int len, bool soft_drop, bool urgent = false,
                      bool control = false, bool ignore_unsent_cap = false,
                      bool control_over_cap = false);

    std::unique_ptr<HttpContext> context_;
    TimeStamp timestamp_;
};
