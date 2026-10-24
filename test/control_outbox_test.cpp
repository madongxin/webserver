#include "ControlResponseOutbox.h"
#include "ReplyStage.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int fails = 0;

void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        ++fails;
    }
}

struct Pipe {
    std::vector<std::string> sent;
    int fail_left = 0;
    bool closed = false;
    int drops = 0;
};

ControlSendEnv Env(Pipe *pipe) {
    ControlSendEnv env;
    env.try_send = [pipe](const std::string &frame) -> int {
        if (pipe->closed)
            return -1;
        if (pipe->fail_left > 0) {
            --pipe->fail_left;
            return 0;
        }
        pipe->sent.push_back(frame);
        return 1;
    };
    env.drop_moves = [pipe]() -> size_t {
        ++pipe->drops;
        return 0;
    };
    env.close_conn = [pipe]() { pipe->closed = true; };
    return env;
}

uint64_t Metric(const std::string &text, const std::string &key) {
    size_t pos = 0;
    while (pos < text.size()) {
        const bool line_start = pos == 0 || text[pos - 1] == '\n';
        if (line_start && text[pos] != '#' && text.compare(pos, key.size(), key) == 0) {
            const auto line_end = text.find('\n', pos);
            const auto line =
                text.substr(pos, line_end == std::string::npos ? std::string::npos : line_end - pos);
            const auto sp = line.rfind(' ');
            if (sp == std::string::npos)
                return 0;
            return static_cast<uint64_t>(std::strtoull(line.c_str() + sp + 1, nullptr, 10));
        }
        const auto nl = text.find('\n', pos);
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return 0;
}

}  // namespace

int main() {
    {
        Pipe pipe;
        pipe.fail_left = 100;
        Expect(ControlResponseOutbox::Instance().Submit(71, 1, "move", "A", Env(&pipe)) == 0,
               "failed move reply stays queued");
        Expect(pipe.sent.empty(), "failed reply is not delivered");
        Expect(ControlResponseOutbox::Instance().Depth(71) == 1, "one reply waiting");
        Expect(ControlResponseOutbox::Instance().BlocksMove(71), "move pauses while a reply waits");
        Expect(ControlResponseOutbox::Instance().Submit(71, 2, "move", "B", Env(&pipe)) == 0,
               "later reply queues");
        Expect(pipe.sent.empty(), "later reply does not jump ahead");
        Expect(ControlResponseOutbox::Instance().Depth(71) == 2, "both replies wait");
        pipe.fail_left = 0;
        ControlResponseOutbox::Instance().OnBufferDrained(71);
        Expect(pipe.sent.size() == 2 && pipe.sent[0] == "A" && pipe.sent[1] == "B",
               "resend keeps reply order");
        Expect(ControlResponseOutbox::Instance().Depth(71) == 0, "queue drains");
        Expect(!ControlResponseOutbox::Instance().BlocksMove(71), "move resumes after replies");
        ControlResponseOutbox::Instance().Forget(71);
    }

    {
        Pipe pipe;
        pipe.fail_left = 1000000;
        bool filled = true;
        for (uint64_t seq = 1; seq <= ControlResponseOutbox::kMaxFrames; ++seq) {
            if (ControlResponseOutbox::Instance().Submit(72, seq, "heartbeat", "h", Env(&pipe)) != 0)
                filled = false;
        }
        Expect(filled, "replies up to the cap stay queued");
        Expect(ControlResponseOutbox::Instance().Depth(72) == ControlResponseOutbox::kMaxFrames,
               "at cap");
        Expect(ControlResponseOutbox::Instance().Submit(72, 1000, "move", "overflow", Env(&pipe)) == -1,
               "over cap closes");
        Expect(pipe.closed, "overflow disconnects");
        Expect(pipe.sent.empty(), "overflow does not dump replies out of order");
        Expect(pipe.drops > 0, "overflow drops pending move first");
        const auto text = ControlResponseOutbox::Prometheus();
        Expect(Metric(text, "gamemesh_control_outbox_overflow_total ") >= 1, "overflow counted");
        Expect(Metric(text, "gamemesh_control_outbox_peak_frames ") >= ControlResponseOutbox::kMaxFrames,
               "frame peak recorded");
        ControlResponseOutbox::Instance().Forget(72);
    }

    {
        ReplyStage::NoteRequest(73, 9, "move");
        Pipe pipe;
        Expect(ControlResponseOutbox::Instance().Submit(73, 9, "move", "ok", Env(&pipe)) == 1,
               "reply writes when the buffer accepts it");
        Expect(pipe.sent.size() == 1 && pipe.sent[0] == "ok", "accepted reply is sent");
        const auto text = ReplyStage::Prometheus();
        Expect(Metric(text, "gamemesh_response_soft_drop_total ") == 0, "no silent reply drop");
        Expect(Metric(text, "gamemesh_reply_stage_total{kind=\"move\",stage=\"socket_write_accepted\"} ") >=
                   1,
               "socket accept counted");
        Expect(Metric(text, "gamemesh_reply_stage_total{kind=\"move\",stage=\"response_soft_drop\"} ") == 0,
               "soft-drop stage stays zero");
        Expect(Metric(text, "gamemesh_reply_latency_ms_count{kind=\"move\"} ") >= 1,
               "move latency sample recorded");
        const auto p50 = Metric(text, "gamemesh_reply_latency_p50_ms{kind=\"move\"} ");
        const auto p95 = Metric(text, "gamemesh_reply_latency_p95_ms{kind=\"move\"} ");
        const auto p99 = Metric(text, "gamemesh_reply_latency_p99_ms{kind=\"move\"} ");
        Expect(p50 >= 5 && p95 >= p50 && p99 >= p95, "percentiles are bucket edges from the sample");
        ControlResponseOutbox::Instance().Forget(73);
    }

    {
        Pipe pipe;
        pipe.fail_left = 8;
        const std::string big(ControlResponseOutbox::kMaxBytes + 1, 'x');
        Expect(ControlResponseOutbox::Instance().Submit(74, 1, "snapshot", big, Env(&pipe)) == -1,
               "reply over the byte cap closes");
        Expect(pipe.closed, "byte cap disconnects");
        Expect(pipe.sent.empty(), "oversized reply is not written");
        ControlResponseOutbox::Instance().Forget(74);
    }

    if (fails != 0) {
        std::printf("control_outbox_test FAIL %d\n", fails);
        return 1;
    }
    std::printf("control_outbox_test OK\n");
    return 0;
}
