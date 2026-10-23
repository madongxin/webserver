#pragma once

#include <cstdint>
#include <functional>
#include <string>

/**
 * Gateway PushBatch 对单条消息的判定。
 * 顺序固定：连接 / 玩家 / 网关 / fence / generation / 帧长，通过之后才允许分配可靠序号。
 * 序号一旦分配就留在回放里；发送失败记 REPLAY_PENDING，不回滚。
 */
enum class PushAdmitReason : int {
    Ok = 0,
    NoSession = 1,
    PlayerMismatch = 2,
    GatewayMismatch = 3,
    FenceMismatch = 4,
    GenerationMismatch = 5,
    SeqAllocationFailed = 6,
    BadFrame = 7,
    SendFailed = 8,
    Closed = 9,
    ReplayPending = 10,
};

struct PushBindView {
    bool found = false;
    uint64_t player_id = 0;
    std::string gateway_instance_id;
    std::string token;
    uint64_t generation = 0;
    bool has_sender = false;
};

struct PushMsgView {
    uint64_t player_id = 0;
    std::string fence_token;
    uint64_t generation = 0;
    bool reliable = false;
    uint64_t server_seq = 0;
    std::string payload;
};

struct PushAdmitEnv {
    std::string local_gateway_id;
    /** 只在校验通过且需要新序号时调用。返回 0 表示分配失败。 */
    std::function<uint64_t()> assign_seq;
    /** 用已分配的 seq 组帧。失败且序号已分配时记为待回放。 */
    std::function<bool(uint64_t seq, std::string *frame)> encode;
    /**
     * 1 = 已进入发送缓冲或合并槽。
     * 0 = 还连着，但这一帧没进去。
     * <0 = 连接已经没了。
     */
    std::function<int(const std::string &frame)> send;
};

struct PushAdmitOut {
    PushAdmitReason reason = PushAdmitReason::Ok;
    bool queued = false;
    bool replay_pending = false;
    uint64_t server_seq = 0;
    int send_code = 0;
};

PushAdmitOut AdmitPushItem(const PushMsgView &msg, const PushBindView &bind, const PushAdmitEnv &env);

inline const char *PushAdmitReasonName(PushAdmitReason reason) {
    switch (reason) {
    case PushAdmitReason::Ok:
        return "ok";
    case PushAdmitReason::NoSession:
        return "no_session";
    case PushAdmitReason::PlayerMismatch:
        return "player_mismatch";
    case PushAdmitReason::GatewayMismatch:
        return "gateway_mismatch";
    case PushAdmitReason::FenceMismatch:
        return "fence_mismatch";
    case PushAdmitReason::GenerationMismatch:
        return "generation_mismatch";
    case PushAdmitReason::SeqAllocationFailed:
        return "seq_allocation_failed";
    case PushAdmitReason::BadFrame:
        return "bad_frame";
    case PushAdmitReason::SendFailed:
        return "send_failed";
    case PushAdmitReason::Closed:
        return "closed";
    case PushAdmitReason::ReplayPending:
        return "replay_pending";
    }
    return "unspecified";
}
