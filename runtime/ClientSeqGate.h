#pragma once

/**
 * @file ClientSeqGate.h
 * @brief 每玩家 client_seq：重复幂等、回退拒绝（与 BoundPlayer 状态解耦，便于单测）
 *
 * GameRequest.seq 是连接级 RPC 序号。Hello / Heartbeat / PushAck 在 Gateway
 * 本地应答，不会 Dispatch 到 GameLogic，因此 Logic 侧看到的 seq 允许空洞
 * （例如 EnterMap=4、Heartbeat=5、Move=6）。只拒绝 client_seq < last（陈旧重放）。
 */

#include <cstdint>
#include <string>

enum class ClientSeqDecision { Execute, Idempotent, Reject };

inline ClientSeqDecision EvaluateClientSeq(uint64_t last_client_seq, uint64_t client_seq,
                                           const std::string &cached_frame,
                                           std::string *out_cached, std::string *err) {
    if (client_seq == 0)
        return ClientSeqDecision::Execute;
    if (client_seq == last_client_seq && last_client_seq != 0) {
        if (out_cached)
            *out_cached = cached_frame;
        return ClientSeqDecision::Idempotent;
    }
    if (last_client_seq != 0 && client_seq < last_client_seq) {
        if (err)
            *err = "ERR_STALE_SEQ";
        return ClientSeqDecision::Reject;
    }
    return ClientSeqDecision::Execute;
}
