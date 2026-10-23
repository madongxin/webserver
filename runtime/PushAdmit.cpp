#include "PushAdmit.h"

namespace {

PushAdmitOut Fail(PushAdmitReason reason, uint64_t seq, bool replay) {
    PushAdmitOut out;
    out.reason = reason;
    out.server_seq = seq;
    out.replay_pending = replay;
    return out;
}

}  // namespace

PushAdmitOut AdmitPushItem(const PushMsgView &msg, const PushBindView &bind, const PushAdmitEnv &env) {
    if (!bind.found)
        return Fail(PushAdmitReason::NoSession, 0, false);
    if (!bind.has_sender)
        return Fail(PushAdmitReason::Closed, 0, false);
    if (bind.player_id != 0 && msg.player_id != 0 && bind.player_id != msg.player_id)
        return Fail(PushAdmitReason::PlayerMismatch, 0, false);
    if (!env.local_gateway_id.empty() && !bind.gateway_instance_id.empty() &&
        bind.gateway_instance_id != env.local_gateway_id)
        return Fail(PushAdmitReason::GatewayMismatch, 0, false);
    if (!msg.fence_token.empty() && !bind.token.empty() && msg.fence_token != bind.token)
        return Fail(PushAdmitReason::FenceMismatch, 0, false);
    if (msg.generation != 0 && bind.generation != 0 && msg.generation != bind.generation)
        return Fail(PushAdmitReason::GenerationMismatch, 0, false);
    if (msg.payload.size() < 4)
        return Fail(PushAdmitReason::BadFrame, 0, false);

    uint64_t seq = msg.server_seq;
    if (msg.reliable && seq == 0) {
        if (!env.assign_seq)
            return Fail(PushAdmitReason::SeqAllocationFailed, 0, false);
        seq = env.assign_seq();
        if (seq == 0)
            return Fail(PushAdmitReason::SeqAllocationFailed, 0, false);
    }

    std::string frame;
    if (!env.encode || !env.encode(seq, &frame) || frame.empty()) {
        if (msg.reliable && seq != 0)
            return Fail(PushAdmitReason::ReplayPending, seq, true);
        return Fail(PushAdmitReason::BadFrame, seq, false);
    }
    if (!env.send) {
        if (msg.reliable && seq != 0)
            return Fail(PushAdmitReason::ReplayPending, seq, true);
        return Fail(PushAdmitReason::Closed, seq, false);
    }
    const int code = env.send(frame);
    if (code > 0) {
        PushAdmitOut out;
        out.reason = PushAdmitReason::Ok;
        out.queued = true;
        out.server_seq = seq;
        out.send_code = code;
        return out;
    }
    if (msg.reliable && seq != 0) {
        PushAdmitOut out = Fail(PushAdmitReason::ReplayPending, seq, true);
        out.send_code = code;
        return out;
    }
    PushAdmitOut out = Fail(code < 0 ? PushAdmitReason::Closed : PushAdmitReason::SendFailed, seq, false);
    out.send_code = code;
    return out;
}
