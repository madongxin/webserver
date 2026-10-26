#pragma once

#include <cstdint>
#include <string>

/**
 * RPC 回包从收到请求到写进发送缓冲的分阶段计数。
 * p50/p95/p99 是进程启动以来延迟桶的上沿，不是估算出来的空数。
 * 没有样本时分位数为 0。
 */
class ReplyStage {
public:
    enum Stage {
        RequestReceived = 0,
        LogicDispatchStarted,
        LogicResponseReady,
        EventLoopEnqueued,
        ControlOutboxQueued,
        SocketWriteAccepted,
        ResponseSoftDrop,
        GatewayCallbackReceived,
        WriteComplete,
        ConnectionClosedBeforeReply,
        EventLoopEnqueueFailed,
        OutboxWakeupMissed,
        StageCount
    };

    static void Inc(const char *kind, Stage stage);
    /** seq 为 0 时不记录。同一条连接上后到的同序号覆盖先到的。 */
    static void NoteRequest(uint64_t conn_id, uint64_t seq, const char *kind);
    /** 写进发送缓冲时结算。没有对应的 NoteRequest 就不记延迟。 */
    static void NoteWritten(uint64_t conn_id, uint64_t seq, const char *kind);
    static std::string Prometheus();
};
