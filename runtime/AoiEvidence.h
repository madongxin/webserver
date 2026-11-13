#pragma once

#include <cstdint>
#include <ostream>
#include <string>

/**
 * 压测取证。只计数、采样和导出，不决定一帧发不发。
 * 没有采到的量不要写成 0：Prometheus 里就不输出那条序列，导出 JSON 用 null。
 */

enum class AoiWriteFrame : int {
    Enter = 0,
    Leave,
    Move,
    Reply,
    Heartbeat,
    Snapshot,
    Reliable,
    Other,
    Count
};

enum class AoiWriteFailReason : int {
    Blocked = 0,
    QueueFull,
    TimedWait,
    Closed,
    NoSession,
    Suppress,
    Count
};

enum class AoiMoveStage : int {
    SerialQueue = 0,
    LockWait,
    LockHold,
    MoveExec,
    ReplyQueue,
    ReplySend,
    PresentSlice,
    Count
};

const char *AoiWriteFrameName(AoiWriteFrame frame);
const char *AoiWriteFailReasonName(AoiWriteFailReason reason);
const char *AoiMoveStageName(AoiMoveStage stage);

/** 默认关。GAMEMESH_AOI_TRACE=1 才记。热路径上先看这个，关着就不再解析。 */
bool AoiTraceEnabled();

void AoiNoteWriteFail(uint64_t conn_id, AoiWriteFrame frame, AoiWriteFailReason reason);
void AoiNoteTxBytes(AoiWriteFrame frame, uint64_t bytes, uint64_t conn_id);

/** 一帧里每种 op 各记一次。解析不了就记 Other。 */
void AoiNoteFrameWrite(uint64_t conn_id, const std::string &frame, bool ok, AoiWriteFailReason reason,
                       bool heartbeat);

void AoiNoteLeaveWriteFail(uint64_t viewer_id, uint64_t subject_id, int64_t now_ms);
/** 这条失败是什么时候开始的。没有记录返回 0。补发不会改这个时间。 */
int64_t AoiLeaveFailSince(uint64_t viewer_id, uint64_t subject_id);
/** 观察者已下线。删掉以他为接收者的失败记录，并计入 viewer_offline。 */
void AoiDropViewerLeavePending(uint64_t viewer_id);
/** 同上，只删一对。记录已经不在时不加计数。 */
void AoiNoteLeaveDroppedOffline(uint64_t viewer_id, uint64_t subject_id);
/**
 * 逻辑服看不到网关 Send-Q。没注册时返回 -1（无数据）。
 * 测试或同进程调用可以注册。大于 16384 时 LEAVE 补发不进入写路径。
 */
using AoiViewerUnsentFn = int (*)(uint64_t viewer_id);
void AoiSetViewerUnsentFnForTest(AoiViewerUnsentFn fn);
int AoiViewerUnsentBytes(uint64_t viewer_id);
void AoiNoteLeaveWriteOk(uint64_t viewer_id, uint64_t subject_id, int64_t now_ms);
void AoiNoteLeaveGiveUp(uint64_t viewer_id, uint64_t subject_id);

void AoiNoteTombstoneReject(uint64_t entity_id);
void AoiNoteTombstoneEnter(uint64_t entity_id);

void AoiNoteLeaveCells(int reason, int chebyshev_cells);

void AoiNoteMoveStage(AoiMoveStage stage, int64_t us, bool unsent_over_16kb, bool unsent_known);
void AoiNotePresentSlice(int64_t us, int entities);

void AoiEnterMoveTiming();
void AoiLeaveMoveTiming();
bool AoiInMoveTiming();

struct AoiTraceFields {
    uint64_t viewer = 0;
    uint64_t subject = 0;
    int op = 0;
    uint64_t version = 0;
    int64_t offer_ms = 0;
    int64_t buffer_ms = 0;
    int64_t kernel_ms = 0;
    int fail = 0;
};

void AoiTrace(const AoiTraceFields &ev);

/**
 * 未发送字节超过 16KB 时采样。同一条连接的 TCP_INFO 最多每秒 1 次。
 * 本机头文件没有的字段保持“无数据”，不写成 0。
 * 假的读取函数只给单测用。生产路径不设置它。
 */
struct AoiTcpSample {
    bool ok = false;
    bool retransmitting = false;
    bool has_backoff = false;
    bool has_last_ack_recv = false;
    bool has_last_data_sent = false;
    bool has_bytes_acked = false;
    bool has_bytes_retrans = false;
    bool has_total_retrans = false;
    bool has_notsent_ioctl = false;
    int notsent = -1;
    uint32_t unacked = 0;
    uint32_t retrans = 0;
    uint32_t backoff = 0;
    uint32_t rto_us = 0;
    uint32_t rtt_us = 0;
    uint32_t snd_cwnd = 0;
    uint32_t last_ack_recv_ms = 0;
    uint32_t last_data_sent_ms = 0;
    uint64_t bytes_acked = 0;
    uint64_t bytes_retrans = 0;
    uint32_t total_retrans = 0;
};

using AoiTcpSampleFn = bool (*)(int fd, AoiTcpSample *out);
void AoiSetTcpSampleFnForTest(AoiTcpSampleFn fn);
void AoiSetEvidenceNowMsForTest(int64_t ms);

using AoiConnPlayerFn = bool (*)(uint64_t conn_id, uint64_t *player_id);
void AoiSetConnPlayerFn(AoiConnPlayerFn fn);

/** 返回 false 表示这进程不知道该观察者在不在线，拆分指标就不输出。 */
using AoiViewerOnlineFn = bool (*)(uint64_t viewer_id, bool *online);
void AoiSetViewerOnlineFn(AoiViewerOnlineFn fn);

void AoiObserveBusySocket(uint64_t conn_id, int fd, int app_bytes, int unsent_bytes);

void AoiEvidenceWritePrometheus(std::ostream &os);

/** 帧解析在接受 ENTER 或全量快照时通知采样器。不改变发送。 */
void AoiNoteCoincidentEnter(bool enter, bool snapshot);

std::string AoiTraceJson(uint64_t viewer, uint64_t subject, int limit);
std::string AoiVisibleJson();
/** 只读。在管理线程上拼 JSON，不在连接 EventLoop 上调用。 */
std::string AoiStalledJson(int n);

int AoiLeavePendingForViewer(uint64_t viewer_id);
int AoiLeavePendingCount();
uint64_t AoiLeaveGivenUpCount();
uint64_t AoiLeaveDroppedOfflineCount();
void AoiResetEvidenceForTest();
