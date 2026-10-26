#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

/**
 * 一条连接上还没进入 TcpConnection 发送流的 RPC 回包。
 * 先到的回包先写。写进发送流（直接写出或进了应用缓冲）就算完成，继续写下一条。
 * 只有这次没写进去才留在队列里。不允许在 TCP 还活着的时候把回包丢掉。
 */
struct ControlSendEnv {
    /**
     * 1 = 整帧已经离开应用缓冲。
     * 2 = 帧已按序进入应用缓冲，下一条可以继续写。
     * 0 = 这次没写进去，连接还在。小于 0 = 连接没了。
     */
    std::function<int(const std::string &frame)> try_send;
    /** 丢掉这条连接上还没写进内核的 AOI MOVE。返回丢掉的帧数。 */
    std::function<size_t()> drop_moves;
    std::function<void()> close_conn;
    /**
     * 写不进去时预约一次重试。必须异步执行，不能在调用里直接跑回调。
     * 同一条连接上一次只保留一个预约。
     */
    std::function<void(std::function<void()>)> arm_retry;
};

class ControlResponseOutbox {
public:
    static constexpr size_t kMaxFrames = 512;
    static constexpr size_t kMaxBytes = 2 * 1024 * 1024;

    static ControlResponseOutbox &Instance();

    /**
     * 只在该连接的 EventLoop 上调用。
     * 1 = 整帧已经离开应用缓冲。
     * 2 = 这一帧已进入发送流，后面的回包也已继续写，或发送流里已有这一帧。
     * 0 = 还没进入发送流，留在队列里。
     * 小于 0 = 队列超过上限，连接已关闭，或连接已经没了。
     */
    int Submit(uint64_t conn_id, uint64_t seq, const char *kind, std::string frame,
               ControlSendEnv env);
    void OnBufferDrained(uint64_t conn_id);
    void Forget(uint64_t conn_id);
    size_t Depth(uint64_t conn_id);
    /** 逐帧等待已经去掉。已进入发送流的回包不再挡住下一条。 */
    bool Holding(uint64_t conn_id);
    /** 队列里还有还没进入 TcpConnection 发送流的回包。已接受的缓冲回包不挡 MOVE。 */
    bool BlocksMove(uint64_t conn_id);

    using DiscardFn = void (*)(uint64_t conn_id);
    static void SetPendingMoveDiscard(DiscardFn fn);

    static std::string Prometheus();

private:
    int Drain(uint64_t conn_id);
};
