#pragma once

/**
 * @file ClientSeqGate.h
 * @brief 每玩家 client_seq：重复幂等、乱序拒绝（与 BoundPlayer 状态解耦，便于单测）
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
    if (last_client_seq != 0 && client_seq != last_client_seq + 1) {
        if (err)
            *err = "ERR_CLIENT_SEQ_OUT_OF_ORDER";
        return ClientSeqDecision::Reject;
    }
    return ClientSeqDecision::Execute;
}
