#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

/**
 * 一条连接上还没写进发送缓冲的 RPC 回包。
 * 先到的回包先写。写不进去就留在队列里，连接可写后再按顺序补发。
 * 不允许在 TCP 还活着的时候把回包丢掉。
 */
struct ControlSendEnv {
    /** 1 = 已经写进发送缓冲。0 = 这次没写进去，连接还在。小于 0 = 连接没了。 */
    std::function<int(const std::string &frame)> try_send;
    /** 丢掉这条连接上还没写进内核的 AOI MOVE。返回丢掉的帧数。 */
    std::function<size_t()> drop_moves;
    std::function<void()> close_conn;
};

class ControlResponseOutbox {
public:
    static constexpr size_t kMaxFrames = 512;
    static constexpr size_t kMaxBytes = 2 * 1024 * 1024;

    static ControlResponseOutbox &Instance();

    /**
     * 只在该连接的 EventLoop 上调用。
     * 1 = 已写进发送缓冲。
     * 0 = 排在更早的回包后面，或这次没写进缓冲，连接还在。
     * 小于 0 = 队列超过上限，连接已关闭，或连接已经没了。
     */
    int Submit(uint64_t conn_id, uint64_t seq, const char *kind, std::string frame,
               ControlSendEnv env);
    void OnBufferDrained(uint64_t conn_id);
    void Forget(uint64_t conn_id);
    size_t Depth(uint64_t conn_id);
    /** 待发队列里还有回包，或刚写进缓冲、还没排空的回包。MOVE 这时不能再进内核。 */
    bool BlocksMove(uint64_t conn_id);

    using DiscardFn = void (*)(uint64_t conn_id);
    static void SetPendingMoveDiscard(DiscardFn fn);

    static std::string Prometheus();

private:
    int Drain(uint64_t conn_id);
};
