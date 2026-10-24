#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

/**
 * 一条连接上还没写进发送缓冲的可靠帧。
 * 入队顺序就是序号顺序：调用方在分配序号到入队之间持有同一把锁。
 * 队头发送失败时留在队头，后面的帧不许先写到 socket。
 */
struct ReliableSendQueue {
    struct Item {
        uint64_t seq = 0;
        std::string frame;
        bool failed = false;
    };

    static constexpr size_t kMaxFrames = 512;
    static constexpr size_t kMaxBytes = 1024 * 1024;

    enum class Add { Head, Behind, Overflow };

    std::deque<Item> q;
    size_t bytes = 0;

    Add AddFrame(uint64_t seq, std::string frame) {
        if (q.size() >= kMaxFrames || bytes + frame.size() > kMaxBytes)
            return Add::Overflow;
        const bool head = q.empty();
        bytes += frame.size();
        Item item;
        item.seq = seq;
        item.frame = std::move(frame);
        q.push_back(std::move(item));
        return head ? Add::Head : Add::Behind;
    }

    void MarkHeadFailed() {
        if (!q.empty())
            q.front().failed = true;
    }

    bool HeadFailed() const { return !q.empty() && q.front().failed; }

    void Pop() {
        if (q.empty())
            return;
        bytes -= q.front().frame.size();
        q.pop_front();
    }
};

class GatewayConnRegistry;

/** 每个连接一把顺序锁。可靠帧先入队，连接可写时再按队头补发。 */
class ReliableOutboxHub {
public:
    struct Order {
        std::shared_ptr<void> keep;
        std::unique_lock<std::mutex> lk;
        ReliableSendQueue *q = nullptr;
        bool *flushing = nullptr;
        bool *blocked = nullptr;
        bool *closed = nullptr;
        uint64_t conn_id = 0;
    };

    static ReliableOutboxHub &Instance();

    Order Lock(uint64_t conn_id);
    /**
     * 调用时持有 order.lk。
     * 1 = 已进入发送缓冲，或排在更早的可靠帧后面。
     * 0 = 连接还在，队头这次没写进缓冲，留在队列里等待补发。
     * <0 = 连接没了，或队列超过上限后已关闭。
     */
    int Accept(Order &order, uint64_t seq, std::string frame);
    void OnBufferDrained(uint64_t conn_id);
    void Forget(uint64_t conn_id);
    /** 这条连接上还没写进发送缓冲的可靠帧数。没有队列时为 0。 */
    size_t Depth(uint64_t conn_id);

private:
    int Drain(uint64_t conn_id);
};
