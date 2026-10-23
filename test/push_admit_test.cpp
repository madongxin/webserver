#include "AoiRecipientGate.h"
#include "PushAdmit.h"

#include <cstdio>
#include <string>

namespace {

int fails = 0;

void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        ++fails;
    }
}

PushBindView LiveBind() {
    PushBindView b;
    b.found = true;
    b.has_sender = true;
    b.player_id = 7;
    b.gateway_instance_id = "gw-0";
    b.token = "fence-a";
    b.generation = 3;
    return b;
}

PushMsgView LiveMsg() {
    PushMsgView m;
    m.player_id = 7;
    m.fence_token = "fence-a";
    m.generation = 3;
    m.reliable = true;
    m.payload = "1234";
    return m;
}

PushAdmitOut Run(const PushMsgView &msg, const PushBindView &bind, int send_code, int *assigns,
                 uint64_t *stored_seq) {
    PushAdmitEnv env;
    env.local_gateway_id = "gw-0";
    env.assign_seq = [&]() {
        if (assigns)
            ++*assigns;
        const uint64_t seq = 11;
        if (stored_seq)
            *stored_seq = seq;
        return seq;
    };
    env.encode = [](uint64_t seq, std::string *frame) {
        if (!frame)
            return false;
        *frame = seq == 0 ? "raw" : "frame";
        return true;
    };
    env.send = [&](const std::string &) { return send_code; };
    return AdmitPushItem(msg, bind, env);
}

}  // namespace

int main() {
    int assigns = 0;
    uint64_t stored = 0;

    {
        PushBindView missing;
        const auto out = Run(LiveMsg(), missing, 1, &assigns, &stored);
        Expect(out.reason == PushAdmitReason::NoSession && !out.queued && assigns == 0 && stored == 0,
               "missing session does not allocate seq");
    }
    {
        auto msg = LiveMsg();
        auto bind = LiveBind();
        msg.fence_token = "other";
        const auto out = Run(msg, bind, 1, &assigns, &stored);
        Expect(out.reason == PushAdmitReason::FenceMismatch && assigns == 0, "fence mismatch");
    }
    {
        auto msg = LiveMsg();
        auto bind = LiveBind();
        msg.generation = 2;
        const auto out = Run(msg, bind, 1, &assigns, &stored);
        Expect(out.reason == PushAdmitReason::GenerationMismatch && assigns == 0, "old generation");
    }
    {
        auto msg = LiveMsg();
        auto bind = LiveBind();
        bind.gateway_instance_id = "gw-1";
        const auto out = Run(msg, bind, 1, &assigns, &stored);
        Expect(out.reason == PushAdmitReason::GatewayMismatch && assigns == 0, "cross gateway");
    }
    {
        auto msg = LiveMsg();
        msg.payload = "x";
        const auto out = Run(msg, LiveBind(), 1, &assigns, &stored);
        Expect(out.reason == PushAdmitReason::BadFrame && assigns == 0, "short frame");
    }
    {
        auto bind = LiveBind();
        bind.has_sender = false;
        const auto out = Run(LiveMsg(), bind, 1, &assigns, &stored);
        Expect(out.reason == PushAdmitReason::Closed && assigns == 0, "no sender");
    }
    {
        const auto out = Run(LiveMsg(), LiveBind(), 0, &assigns, &stored);
        Expect(out.reason == PushAdmitReason::ReplayPending && out.replay_pending && !out.queued,
               "send reject keeps seq for replay");
        Expect(assigns == 1 && stored == 11 && out.server_seq == 11, "seq not rolled back");
    }
    {
        const auto before = stored;
        const auto out = Run(LiveMsg(), LiveBind(), -1, &assigns, &stored);
        Expect(out.reason == PushAdmitReason::ReplayPending && out.replay_pending, "closed after seq");
        Expect(stored == before && out.server_seq == before, "closed does not rewind seq");
    }
    {
        auto msg = LiveMsg();
        msg.reliable = false;
        msg.server_seq = 0;
        const auto out = Run(msg, LiveBind(), 0, &assigns, &stored);
        Expect(out.reason == PushAdmitReason::SendFailed && !out.replay_pending && assigns == 2,
               "unreliable send failure does not allocate seq");
    }
    {
        const auto out = Run(LiveMsg(), LiveBind(), 1, &assigns, &stored);
        Expect(out.queued && out.reason == PushAdmitReason::Ok && out.server_seq == 11,
               "queued after checks");
        Expect(assigns == 3, "success allocates once");
    }
    {
        auto msg = LiveMsg();
        msg.generation = 0;
        msg.fence_token.clear();
        const int before = assigns;
        const auto out = Run(msg, LiveBind(), 1, &assigns, &stored);
        Expect(out.queued && assigns == before + 1, "empty fence and generation are not rejects");
    }

    AoiRecipientGate::Instance().ClearAll();
    Expect(AoiRecipientGate::Instance().Allowed(42), "default allow");
    AoiRecipientGate::Instance().Suppress(42);
    Expect(!AoiRecipientGate::Instance().Allowed(42), "suppress blocks recipient");
    Expect(AoiRecipientGate::Instance().Allowed(43), "other recipient stays");
    AoiRecipientGate::Instance().Clear(42);
    Expect(AoiRecipientGate::Instance().Allowed(42), "bind clears suppress");

    if (fails != 0) {
        std::printf("push_admit_test FAIL %d\n", fails);
        return 1;
    }
    std::printf("push_admit_test OK\n");
    return 0;
}
