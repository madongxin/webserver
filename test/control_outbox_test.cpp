#include "ControlResponseOutbox.h"
#include "ReplyStage.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
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

long long Metric(const std::string &text, const std::string &key) {
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
            return std::strtoll(line.c_str() + sp + 1, nullptr, 10);
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
        Expect(Metric(text, "gamemesh_control_outbox_peak_frames ") >=
                   static_cast<long long>(ControlResponseOutbox::kMaxFrames),
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

    {
        struct Sock {
            std::string app;
            std::vector<std::string> wire;
            int calls = 0;
            bool closed = false;
            int drops = 0;
        } sock;
        ControlSendEnv env;
        env.try_send = [&sock](const std::string &frame) -> int {
            ++sock.calls;
            if (sock.closed)
                return -1;
            sock.app.append(frame);
            return 2;
        };
        env.drop_moves = [&sock]() -> size_t {
            ++sock.drops;
            return 0;
        };
        env.close_conn = [&sock]() { sock.closed = true; };
        Expect(ControlResponseOutbox::Instance().Submit(81, 10, "move", "M1", env) == 2,
               "buffered move enters the send stream");
        Expect(!ControlResponseOutbox::Instance().Holding(81), "buffered reply is not held");
        Expect(!ControlResponseOutbox::Instance().BlocksMove(81), "buffered reply does not block move");
        Expect(ControlResponseOutbox::Instance().Submit(81, 12, "heartbeat", "HB", env) == 2,
               "heartbeat follows the buffered move");
        Expect(ControlResponseOutbox::Instance().Submit(81, 11, "move", "M2", env) == 2,
               "second move follows the heartbeat");
        Expect(sock.calls == 3 && sock.app == "M1HBM2", "heartbeat does not pass the earlier move");
        Expect(ControlResponseOutbox::Instance().Depth(81) == 0, "accepted replies leave the outbox");
        Expect(!sock.closed, "in-order replies do not close the connection");
        ControlResponseOutbox::Instance().Forget(81);
    }

    auto stage = [](const std::string &text, const char *kind, const char *name) {
        return Metric(text, std::string("gamemesh_reply_stage_total{kind=\"") + kind +
                                "\",stage=\"" + name + "\"} ");
    };

    {
        const auto before = ReplyStage::Prometheus();
        ReplyStage::Inc("move", ReplyStage::RequestReceived);
        ReplyStage::Inc("move", ReplyStage::LogicDispatchStarted);
        ReplyStage::Inc("move", ReplyStage::LogicResponseReady);
        ReplyStage::Inc("move", ReplyStage::GatewayCallbackReceived);
        ReplyStage::Inc("move", ReplyStage::EventLoopEnqueued);
        Pipe pipe;
        Expect(ControlResponseOutbox::Instance().Submit(91, 3, "move", "once", Env(&pipe)) == 1,
               "direct reply leaves the app buffer");
        ControlResponseOutbox::Instance().OnBufferDrained(91);
        const auto after = ReplyStage::Prometheus();
        Expect(stage(after, "move", "write_complete") == stage(before, "move", "write_complete") + 1,
               "write_complete counted once");
        Expect(stage(after, "move", "socket_write_accepted") ==
                   stage(before, "move", "socket_write_accepted") + 1,
               "socket accept counted once");
        Expect(stage(after, "move", "connection_closed_before_reply") ==
                   stage(before, "move", "connection_closed_before_reply"),
               "direct write is not a close");
        Expect(Metric(after, "gamemesh_reply_conservation_residual ") ==
                   Metric(before, "gamemesh_reply_conservation_residual "),
               "paired request and write_complete keep the residual");
        ControlResponseOutbox::Instance().Forget(91);
        const auto gone = ReplyStage::Prometheus();
        Expect(stage(gone, "move", "write_complete") == stage(after, "move", "write_complete"),
               "forget does not count write_complete again");
        Expect(stage(gone, "move", "connection_closed_before_reply") ==
                   stage(after, "move", "connection_closed_before_reply"),
               "forget after completion does not count a close");
    }

    {
        const auto before = ReplyStage::Prometheus();
        ReplyStage::Inc("move", ReplyStage::RequestReceived);
        struct Sock {
            std::string app;
            bool closed = false;
        } sock;
        ControlSendEnv env;
        env.try_send = [&sock](const std::string &frame) -> int {
            if (sock.closed)
                return -1;
            sock.app = frame;
            return 2;
        };
        env.drop_moves = []() -> size_t { return 0; };
        env.close_conn = [&sock]() { sock.closed = true; };
        Expect(ControlResponseOutbox::Instance().Submit(92, 4, "move", "held", env) == 2,
               "buffered reply is accepted");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const auto held = ControlResponseOutbox::Prometheus();
        Expect(Metric(held, "gamemesh_control_outbox_oldest_held_ms ") == 0,
               "buffered reply does not age a hold");
        Expect(!ControlResponseOutbox::Instance().BlocksMove(92), "buffered reply does not block move");
        const auto mid = ReplyStage::Prometheus();
        Expect(stage(mid, "move", "write_complete") == stage(before, "move", "write_complete") + 1,
               "buffered reply counts write_complete once");
        sock.app.clear();
        ControlResponseOutbox::Instance().OnBufferDrained(92);
        ControlResponseOutbox::Instance().OnBufferDrained(92);
        const auto after = ReplyStage::Prometheus();
        Expect(stage(after, "move", "write_complete") == stage(mid, "move", "write_complete"),
               "drain does not complete a buffered reply again");
        ControlResponseOutbox::Instance().Forget(92);
        const auto gone = ReplyStage::Prometheus();
        Expect(stage(gone, "move", "connection_closed_before_reply") ==
                   stage(after, "move", "connection_closed_before_reply"),
               "forget after write_complete does not close it");
    }

    {
        const auto before = ReplyStage::Prometheus();
        const auto box_before = ControlResponseOutbox::Prometheus();
        ReplyStage::Inc("heartbeat", ReplyStage::RequestReceived);
        Pipe pipe;
        pipe.fail_left = 8;
        Expect(ControlResponseOutbox::Instance().Submit(93, 5, "heartbeat", "queued", Env(&pipe)) == 0,
               "blocked reply stays queued");
        const auto queued = ControlResponseOutbox::Prometheus();
        Expect(Metric(queued, "gamemesh_control_outbox_current_frames ") >= 1, "current frames");
        Expect(Metric(queued, "gamemesh_control_outbox_drain_stalls_total ") >
                   Metric(box_before, "gamemesh_control_outbox_drain_stalls_total "),
               "drain stall counted");
        Expect(Metric(queued, "gamemesh_control_outbox_drain_runs_total ") >= 1, "drain ran");
        Expect(queued.find("gamemesh_control_outbox_wakeup_missed_total ") != std::string::npos,
               "wakeup metric is exported");
        ControlResponseOutbox::Instance().Forget(93);
        const auto after = ReplyStage::Prometheus();
        Expect(stage(after, "heartbeat", "connection_closed_before_reply") ==
                   stage(before, "heartbeat", "connection_closed_before_reply") + 1,
               "close counts the queued reply");
        Expect(stage(after, "heartbeat", "write_complete") ==
                   stage(before, "heartbeat", "write_complete"),
               "closed reply is not write_complete");
        Expect(Metric(after, "gamemesh_reply_conservation_residual ") ==
                   Metric(before, "gamemesh_reply_conservation_residual "),
               "close keeps the residual");
        const auto empty = ControlResponseOutbox::Prometheus();
        Expect(Metric(empty, "gamemesh_control_outbox_current_frames ") == 0,
               "forget clears the queue");
    }

    {
        struct Gate {
            int deny = 1;
            int arms = 0;
            std::function<void()> pending;
            std::string app;
        } gate;
        ControlSendEnv env;
        env.try_send = [&gate](const std::string &frame) -> int {
            if (gate.deny > 0)
                return 0;
            gate.app.append(frame);
            return 1;
        };
        env.arm_retry = [&gate](std::function<void()> cb) {
            ++gate.arms;
            gate.pending = std::move(cb);
        };
        Expect(ControlResponseOutbox::Instance().Submit(94, 1, "move", "A", env) == 0,
               "blocked move stays queued");
        Expect(gate.arms == 1, "one watchdog armed");
        Expect(ControlResponseOutbox::Instance().BlocksMove(94), "queued reply blocks move");
        Expect(ControlResponseOutbox::Instance().Submit(94, 2, "heartbeat", "B", env) == 0,
               "heartbeat waits behind the blocked move");
        Expect(gate.arms == 1, "watchdog stays one per connection");
        Expect(gate.app.empty(), "blocked replies are not written");
        gate.deny = 0;
        Expect(static_cast<bool>(gate.pending), "watchdog callback stored");
        gate.pending();
        Expect(gate.app == "AB", "watchdog sends move before heartbeat");
        Expect(ControlResponseOutbox::Instance().Depth(94) == 0, "watchdog clears the outbox");
        Expect(!ControlResponseOutbox::Instance().BlocksMove(94), "move resumes after the queue drains");
        Expect(gate.arms == 1, "success does not arm another watchdog");
        ControlResponseOutbox::Instance().Forget(94);
    }

    {
        const auto before = ReplyStage::Prometheus();
        struct Conn {
            int deny = 0;
            std::function<void()> pending;
        };
        std::vector<Conn> conns(305);
        for (int i = 0; i < 165; ++i)
            conns[static_cast<size_t>(i)].deny = 1;
        int bad = 0;
        for (int i = 0; i < 305; ++i) {
            Conn *conn = &conns[static_cast<size_t>(i)];
            ControlSendEnv env;
            env.try_send = [conn](const std::string &) -> int {
                if (conn->deny > 0)
                    return 0;
                return 1;
            };
            env.arm_retry = [conn](std::function<void()> cb) { conn->pending = std::move(cb); };
            const int rc = ControlResponseOutbox::Instance().Submit(
                static_cast<uint64_t>(1000 + i), static_cast<uint64_t>(i + 1), "move", "m", env);
            if ((i < 165 && rc != 0) || (i >= 165 && rc != 1))
                ++bad;
        }
        Expect(bad == 0, "165 conns queue on EAGAIN and the rest are accepted");
        for (int i = 0; i < 165; ++i) {
            conns[static_cast<size_t>(i)].deny = 0;
            Expect(static_cast<bool>(conns[static_cast<size_t>(i)].pending), "blocked conn has one retry");
            conns[static_cast<size_t>(i)].pending();
        }
        bool empty = true;
        for (int i = 0; i < 305; ++i) {
            if (ControlResponseOutbox::Instance().Depth(static_cast<uint64_t>(1000 + i)) != 0 ||
                ControlResponseOutbox::Instance().BlocksMove(static_cast<uint64_t>(1000 + i)))
                empty = false;
            ControlResponseOutbox::Instance().Forget(static_cast<uint64_t>(1000 + i));
        }
        Expect(empty, "305 conns drain after EAGAIN recovers");
        const auto after = ReplyStage::Prometheus();
        Expect(stage(after, "move", "connection_closed_before_reply") ==
                   stage(before, "move", "connection_closed_before_reply"),
               "recovered replies are not closed");
        Expect(stage(after, "move", "write_complete") ==
                   stage(before, "move", "write_complete") + 305,
               "each recovered reply completes once");
        const auto box = ControlResponseOutbox::Prometheus();
        Expect(Metric(box, "gamemesh_control_outbox_current_frames ") == 0, "outbox empty after recovery");
    }

    {
        const char *names[] = {
            "request_received",
            "logic_dispatch_started",
            "logic_response_ready",
            "event_loop_enqueued",
            "control_outbox_queued",
            "socket_write_accepted",
            "response_soft_drop",
            "gateway_callback_received",
            "write_complete",
            "connection_closed_before_reply",
            "event_loop_enqueue_failed",
            "outbox_wakeup_missed",
        };
        ReplyStage::Inc("snapshot", ReplyStage::EventLoopEnqueueFailed);
        ReplyStage::Inc("snapshot", ReplyStage::OutboxWakeupMissed);
        const auto text = ReplyStage::Prometheus();
        for (const char *name : names) {
            Expect(text.find(std::string("stage=\"") + name + "\"") != std::string::npos, name);
        }
        Expect(Metric(text, "gamemesh_reply_inflight ") >= 0, "inflight exported");
    }

    if (fails != 0) {
        std::printf("control_outbox_test FAIL %d\n", fails);
        return 1;
    }
    std::printf("control_outbox_test OK\n");
    return 0;
}
